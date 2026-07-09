/* $Id: UIMachineViewFullscreen.cpp 112403 2026-01-11 19:29:08Z knut.osmundsen@oracle.com $ */
/** @file
 * VBox Qt GUI - UIMachineViewFullscreen class implementation.
 */

/*
 * Copyright (C) 2010-2026 Oracle and/or its affiliates.
 *
 * This file is part of VirtualBox base platform packages, as
 * available from https://www.virtualbox.org.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation, in version 3 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses>.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */

/* Qt includes: */
#include <QApplication>
#include <QResizeEvent>
#include <QMainWindow>
#include <QSizePolicy>
#ifdef VBOX_WS_MAC
# include <QMenuBar>
#endif

/* GUI includes: */
#include "UIActionPoolRuntime.h"
#include "UIDesktopWidgetWatchdog.h"
#include "UIExtraDataManager.h"
#include "UIFrameBuffer.h"
#include "UILoggingDefs.h"
#include "UIMachine.h"
#include "UIMachineLogicFullscreen.h"
#include "UIMachineViewFullscreen.h"
#include "UIMachineWindow.h"

/* External includes: */
#ifdef VBOX_WS_NIX
# include <limits.h>
#endif


UIMachineViewFullscreen::UIMachineViewFullscreen(UIMachineWindow *pMachineWindow, ulong uScreenId)
    : UIMachineView(pMachineWindow, uScreenId)
    , m_fGuestAutoresizeEnabled(actionPool()->action(UIActionIndexRT_M_View_T_GuestAutoresize)->isChecked())
{
}

void UIMachineViewFullscreen::sltAdditionsStateChanged()
{
    adjustGuestScreenSize();
}

void UIMachineViewFullscreen::sltHandleNotifyChange(int iWidth, int iHeight)
{
    UIMachineView::sltHandleNotifyChange(iWidth, iHeight);

#ifdef VBOX_WS_MAC
    applyMacOSFullscreenLayout(calculateMaxGuestSize());
#endif
}

#ifdef VBOX_WS_MAC
void UIMachineViewFullscreen::applyMacOSFullscreenLayout(const QSize &hostSize)
{
    if (!hostSize.isValid() || hostSize.isEmpty())
        return;

    setMinimumSize(hostSize);
    setMaximumSize(hostSize);
    resize(hostSize);

    if (frameBuffer())
    {
        QSize scaledSize = hostSize;
        if (frameBuffer()->useUnscaledHiDPIOutput())
            scaledSize *= frameBuffer()->devicePixelRatio();
        frameBuffer()->setScaledSize(scaledSize);
        frameBuffer()->performRescale();
    }

    updateGeometry();
    updateSliders();
    viewport()->update();
}

void UIMachineViewFullscreen::requestMacOSFullscreenGuestSize(const QSize &hostSize)
{
    if (!frameBuffer())
        return;
    if (   !uimachine()->isRunning()
        && !uimachine()->isPaused())
        return;
    if (!uimachine()->isScreenVisible(screenId()))
        return;

    const QSize hostScreenSize = hostSize.isValid() && !hostSize.isEmpty() ? hostSize : calculateMaxGuestSize();
    if (!hostScreenSize.isValid() || hostScreenSize.isEmpty())
        return;

    /*
     * The fullscreen window can report transient content sizes while macOS is
     * entering/leaving native fullscreen.  Use the host-screen geometry and
     * scale it back to the guest framebuffer size so Retina fullscreen requests
     * the backing resolution (for example 5760x3240 for a 2880x1620 display).
     */
    const QSize guestSize = scaledBackward(hostScreenSize);
    if (!guestSize.isValid() || guestSize.isEmpty())
        return;

    setMaximumGuestSize(guestSize);

    const QSize frameBufferSize(frameBuffer()->width(), frameBuffer()->height());
    if (frameBufferSize == guestSize && requestedGuestScreenSizeHint() == guestSize)
    {
        LogRel(("GUI: UIMachineViewFullscreen::requestMacOSFullscreenGuestSize: "
                "Omitting size-hint %dx%d for guest-screen %d because it is already active.\n",
                guestSize.width(), guestSize.height(), (int)screenId()));
        return;
    }

    LogRel(("GUI: UIMachineViewFullscreen::requestMacOSFullscreenGuestSize: "
            "Sending macOS fullscreen size-hint to guest-screen %d as %dx%d for host size %dx%d\n",
            (int)screenId(), guestSize.width(), guestSize.height(),
            hostScreenSize.width(), hostScreenSize.height()));
    uimachine()->setVideoModeHint(screenId(),
                                  true /* enabled? */,
                                  false /* change origin? */,
                                  0 /* origin x */, 0 /* origin y */,
                                  (ulong)guestSize.width(), (ulong)guestSize.height(),
                                  0 /* bits per pixel */,
                                  true /* notify? */);
}
#endif /* VBOX_WS_MAC */

bool UIMachineViewFullscreen::eventFilter(QObject *pWatched, QEvent *pEvent)
{
    if (pWatched != 0 && pWatched == machineWindow())
    {
        switch (pEvent->type())
        {
            case QEvent::Resize:
            {
                QResizeEvent *pResizeEvent = static_cast<QResizeEvent*>(pEvent);
#ifdef VBOX_WS_MAC
                /* Let the view fill the native full-screen screen immediately.
                 * The Qt window size can lag or temporarily include toolbar
                 * adjustments during the macOS fullscreen transition. */
                Q_UNUSED(pResizeEvent);
                const QSize hostScreenSize = calculateMaxGuestSize();
                applyMacOSFullscreenLayout(hostScreenSize);

                /* Recalculate maximum guest size: */
                setMaximumGuestSize();
                if (m_fGuestAutoresizeEnabled && uimachine()->isGuestSupportsGraphics())
                    requestMacOSFullscreenGuestSize(hostScreenSize);
#else /* !VBOX_WS_MAC */
                /* Send guest-resize hint only if top window resizing to required dimension: */
                if (pResizeEvent->size() != calculateMaxGuestSize())
                    break;

                /* Recalculate maximum guest size: */
                setMaximumGuestSize();
#endif /* !VBOX_WS_MAC */

                break;
            }
            default:
                break;
        }
    }

    return UIMachineView::eventFilter(pWatched, pEvent);
}

void UIMachineViewFullscreen::prepareCommon()
{
    /* Base class common settings: */
    UIMachineView::prepareCommon();

#ifdef VBOX_WS_MAC
    /* Keep the fullscreen view pinned to the host screen instead of transient
     * window contents during macOS native fullscreen transitions. */
    setSizePolicy(QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding));
    setMinimumSize(0, 0);
    applyMacOSFullscreenLayout(calculateMaxGuestSize());
#else /* !VBOX_WS_MAC */
    /* Setup size-policy: */
    setSizePolicy(QSizePolicy(QSizePolicy::Maximum, QSizePolicy::Maximum));
    /* Maximum size to sizehint: */
    setMaximumSize(sizeHint());
    /* Minimum size is ignored: */
    setMinimumSize(0, 0);
#endif /* !VBOX_WS_MAC */
    /* No scrollbars: */
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
}

void UIMachineViewFullscreen::prepareFilters()
{
    /* Base class filters: */
    UIMachineView::prepareFilters();
}

void UIMachineViewFullscreen::prepareConsoleConnections()
{
    /* Base class connections: */
    UIMachineView::prepareConsoleConnections();

    /* Guest additions state-change updater: */
    connect(uimachine(), &UIMachine::sigAdditionsStateActualChange, this, &UIMachineViewFullscreen::sltAdditionsStateChanged);
}

void UIMachineViewFullscreen::setGuestAutoresizeEnabled(bool fEnabled)
{
    if (m_fGuestAutoresizeEnabled != fEnabled)
    {
        m_fGuestAutoresizeEnabled = fEnabled;

        if (m_fGuestAutoresizeEnabled && uimachine()->isGuestSupportsGraphics())
        {
#ifdef VBOX_WS_MAC
            requestMacOSFullscreenGuestSize(calculateMaxGuestSize());
#else
            sltPerformGuestResize();
#endif
        }
    }
}

QSize UIMachineViewFullscreen::sizeHint() const
{
#ifdef VBOX_WS_MAC
    const QSize size = calculateMaxGuestSize();
    if (size.isValid() && !size.isEmpty())
        return size;
#endif
    return UIMachineView::sizeHint();
}

void UIMachineViewFullscreen::adjustGuestScreenSize()
{
    /* Step 0: Is machine running or paused? */
    if (!uimachine()->isRunning() && !uimachine()->isPaused())
    {
        LogRel(("GUI: UIMachineViewFullscreen::adjustGuestScreenSize: "
                "Guest-screen #%d display is not initialized, adjustment is not possible.\n",
                screenId()));
        return;
    }
    /* Step 1: Is guest-screen visible? */
    if (!uimachine()->isScreenVisible(screenId()))
    {
        LogRel(("GUI: UIMachineViewFullscreen::adjustGuestScreenSize: "
                "Guest-screen #%d is not visible, adjustment is not required.\n",
                screenId()));
        return;
    }
    /* Step 2: Is guest-screen auto-resize enabled? */
    if (!isGuestAutoresizeEnabled())
    {
        LogRel(("GUI: UIMachineViewFullscreen::adjustGuestScreenSize: "
                "Guest-screen #%d auto-resize is disabled, adjustment is not required.\n",
                screenId()));
        return;
    }

    /* What are the desired and requested hints? */
    const QSize sizeToApply = calculateMaxGuestSize();
    const QSize desiredSizeHint = scaledBackward(sizeToApply);
    const QSize requestedSizeHint = requestedGuestScreenSizeHint();

    /* Step 3: Is the guest-screen of another size than necessary? */
    if (desiredSizeHint == requestedSizeHint)
    {
        LogRel(("GUI: UIMachineViewFullscreen::adjustGuestScreenSize: "
                "Desired hint %dx%d for guest-screen #%d is already in IDisplay, adjustment is not required.\n",
                desiredSizeHint.width(), desiredSizeHint.height(), screenId()));
        return;
    }

    /* Final step: Adjust .. */
    LogRel(("GUI: UIMachineViewFullscreen::adjustGuestScreenSize: "
            "Desired hint %dx%d for guest-screen #%d differs from the one in IDisplay, adjustment is required.\n",
            desiredSizeHint.width(), desiredSizeHint.height(), screenId()));
#ifdef VBOX_WS_MAC
    requestMacOSFullscreenGuestSize(sizeToApply);
    /* And remember the size to know what we are resizing out of when we exit: */
    uimachine()->setLastFullScreenSize(screenId(), scaledForward(desiredSizeHint));
#else
    sltPerformGuestResize(sizeToApply);
    /* And remember the size to know what we are resizing out of when we exit: */
    uimachine()->setLastFullScreenSize(screenId(), scaledForward(desiredSizeHint));
#endif
}

QRect UIMachineViewFullscreen::workingArea() const
{
    /* Get corresponding screen: */
    int iScreen = static_cast<UIMachineLogicFullscreen*>(machineLogic())->hostScreenForGuestScreen(screenId());
    /* Return available geometry for that screen: */
    return gpDesktop->screenGeometry(iScreen);
}

QSize UIMachineViewFullscreen::calculateMaxGuestSize() const
{
    return workingArea().size();
}
