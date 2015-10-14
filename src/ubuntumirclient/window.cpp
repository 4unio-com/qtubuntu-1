/*
 * Copyright (C) 2014-2015 Canonical, Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License version 3, as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranties of MERCHANTABILITY,
 * SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// Local
#include "clipboard.h"
#include "input.h"
#include "window.h"
#include "screen.h"
#include "logging.h"

// Qt
#include <qpa/qwindowsysteminterface.h>
#include <QtPlatformSupport/private/qeglconvenience_p.h>
#include <QMutex>
#include <QMutexLocker>
#include <QSize>
#include <QtMath>

// Platform API
#include <ubuntu/application/instance.h>

namespace
{
MirSurfaceState qtWindowStateToMirSurfaceState(Qt::WindowState state)
{
    switch (state) {
    case Qt::WindowNoState:
        return mir_surface_state_restored;

    case Qt::WindowFullScreen:
        return mir_surface_state_fullscreen;

    case Qt::WindowMaximized:
        return mir_surface_state_maximized;

    case Qt::WindowMinimized:
        return mir_surface_state_minimized;

    default:
        LOG("Unexpected Qt::WindowState: %d", state);
        return mir_surface_state_restored;
    }
}

#if !defined(QT_NO_DEBUG)
const char *qtWindowStateToStr(Qt::WindowState state)
{
    switch (state) {
    case Qt::WindowNoState:
        return "NoState";

    case Qt::WindowFullScreen:
        return "FullScreen";

    case Qt::WindowMaximized:
        return "Maximized";

    case Qt::WindowMinimized:
        return "Minimized";

    default:
        return "!?";
    }
}
#endif

} // anonymous namespace

class UbuntuWindowPrivate
{
public:
    void createEGLSurface(EGLNativeWindowType nativeWindow);
    void destroyEGLSurface();
    int panelHeight();

    UbuntuScreen* screen;
    EGLSurface eglSurface;
    QSurfaceFormat format;
    WId id;
    UbuntuInput* input;
    Qt::WindowState state;
    MirConnection *connection;
    MirSurface* surface;
    QSize bufferSize;
    QMutex mutex;
    QSharedPointer<UbuntuClipboard> clipboard;
    bool exposed;
    int resizeCatchUpAttempts;
#if !defined(QT_NO_DEBUG)
    int frameNumber;
#endif
};

static void eventCallback(MirSurface* surface, const MirEvent *event, void* context)
{
    (void) surface;
    DASSERT(context != NULL);
    UbuntuWindow* platformWindow = static_cast<UbuntuWindow*>(context);
    platformWindow->priv()->input->postEvent(platformWindow, event);
}

static void surfaceCreateCallback(MirSurface* surface, void* context)
{
    DASSERT(context != NULL);
    UbuntuWindow* platformWindow = static_cast<UbuntuWindow*>(context);
    platformWindow->priv()->surface = surface;

    mir_surface_set_event_handler(surface, eventCallback, context);
}

UbuntuWindow::UbuntuWindow(QWindow* w, QSharedPointer<UbuntuClipboard> clipboard, UbuntuScreen* screen,
                           UbuntuInput* input, MirConnection* connection)
    : QObject(nullptr), QPlatformWindow(w)
{
    DASSERT(screen != NULL);

    d = new UbuntuWindowPrivate;
    d->screen = screen;
    d->eglSurface = EGL_NO_SURFACE;
    d->format = window()->requestedFormat();
    d->input = input;
    d->state = window()->windowState();
    d->connection = connection;
    d->surface = nullptr;
    d->clipboard = clipboard;
    d->resizeCatchUpAttempts = 0;
    d->exposed = true;

    static int id = 1;
    d->id = id++;

#if !defined(QT_NO_DEBUG)
    d->frameNumber = 0;
#endif

    // Use client geometry if set explicitly, use available screen geometry otherwise.
    QPlatformWindow::setGeometry(window()->geometry() != screen->geometry() ?
        window()->geometry() : screen->availableGeometry());
    createWindow();
    DLOG("UbuntuWindow::UbuntuWindow (this=%p, w=%p, screen=%p, input=%p)", this, w, screen, input);
}

UbuntuWindow::~UbuntuWindow()
{
    DLOG("UbuntuWindow::~UbuntuWindow");
    d->destroyEGLSurface();

    mir_surface_release_sync(d->surface);

    delete d;
}

void UbuntuWindowPrivate::destroyEGLSurface()
{
    DLOG("UbuntuWindowPrivate::destroyEGLSurface (this=%p)", this);
    if (eglSurface != EGL_NO_SURFACE) {
        eglDestroySurface(screen->eglDisplay(), eglSurface);
        eglSurface = EGL_NO_SURFACE;
    }
}

// FIXME - in order to work around https://bugs.launchpad.net/mir/+bug/1346633
// we need to guess the panel height (3GU + 2DP)
int UbuntuWindowPrivate::panelHeight()
{
    const int defaultGridUnit = 8;
    int gridUnit = defaultGridUnit;
    QByteArray gridUnitString = qgetenv("GRID_UNIT_PX");
    if (!gridUnitString.isEmpty()) {
        bool ok;
        gridUnit = gridUnitString.toInt(&ok);
        if (!ok) {
            gridUnit = defaultGridUnit;
        }
    }
    qreal densityPixelRatio = static_cast<qreal>(gridUnit) / defaultGridUnit;
    return gridUnit * 3 + qFloor(densityPixelRatio) * 2;
}

// Gets the best pixel format available through that connection. Falls back to an opaque format if
// no satisfying ARGB pixel format can be found. Note that Qt defaults to GL_RGBA.
static MirPixelFormat getPixelFormat(MirConnection *connection, bool hasAlpha)
{
    const unsigned int formatsCount = 5;
    const MirPixelFormat formats[formatsCount] = {
        mir_pixel_format_argb_8888, mir_pixel_format_abgr_8888, mir_pixel_format_xrgb_8888,
        mir_pixel_format_xbgr_8888, mir_pixel_format_bgr_888
    };

    unsigned int availableFormatsCount;
    MirPixelFormat availableFormats[mir_pixel_formats];
    mir_connection_get_available_surface_formats(
        connection, availableFormats, mir_pixel_formats, &availableFormatsCount);

    for (unsigned int i = hasAlpha ? 0 : 2; i < formatsCount; i++) {
        for (unsigned int j = 0; j < availableFormatsCount; j++) {
            if (formats[i] == availableFormats[j]) {
#if !defined(QT_NO_DEBUG)
                const char* formatsName[formatsCount] =
                    { "ARGB_8888", "ABGR_8888", "XRGB_8888", "XBGR_8888", "BGR_888" };
                LOG("best pixel format found for surface is %s", formatsName[i]);
#endif
                return formats[i];
            }
        }
    }

    qWarning("[ubuntumirclient QPA] can't find a valid pixel format");
    return mir_pixel_format_invalid;
}

void UbuntuWindow::createWindow()
{
    DLOG("UbuntuWindow::createWindow (this=%p)", this);

    // FIXME: remove this remnant of an old platform-api enum - needs ubuntu-keyboard update
    const int SCREEN_KEYBOARD_ROLE = 7;
    // Get surface role.
    QVariant roleVariant = window()->property("role");
    int role = roleVariant.isValid() ? roleVariant.toUInt() : 1;  // 1 is the default role for apps.

    const QByteArray title = (!window()->title().isNull()) ? window()->title().toUtf8() : "Window 1"; // legacy title
    const int panelHeight = d->panelHeight();

#if !defined(QT_NO_DEBUG)
    LOG("panelHeight: '%d'", panelHeight);
    LOG("role: '%d'", role);
    LOG("title: '%s'", title.constData());
#endif

    // Get surface geometry.
    QRect geometry;
    if (d->state == Qt::WindowFullScreen) {
        printf("UbuntuWindow - fullscreen geometry\n");
        geometry = screen()->geometry();
    } else if (d->state == Qt::WindowMaximized) {
        printf("UbuntuWindow - maximized geometry\n");
        geometry = screen()->availableGeometry();
        /*
         * FIXME: Autopilot relies on being able to convert coordinates relative of the window
         * into absolute screen coordinates. Mir does not allow this, see bug lp:1346633
         * Until there's a correct way to perform this transformation agreed, this horrible hack
         * guesses the transformation heuristically.
         *
         * Assumption: this method only used on phone devices!
         */
        geometry.setY(panelHeight);
    } else {
        printf("UbuntuWindow - regular geometry\n");
        geometry = this->geometry();
        geometry.setY(panelHeight);
    }

    DLOG("[ubuntumirclient QPA] creating surface at (%d, %d) with size (%d, %d) with title '%s'\n",
            geometry.x(), geometry.y(), geometry.width(), geometry.height(), title.data());

    EGLDisplay eglDisplay = d->screen->eglDisplay();
    EGLConfig eglConfig = q_configFromGLFormat(eglDisplay, d->format, true);
    const bool needsAlpha = d->format.alphaBufferSize() > 0;
    d->format = q_glFormatFromConfig(eglDisplay, eglConfig, d->format);
    MirPixelFormat pixelFormat = getPixelFormat(d->connection, needsAlpha);

    MirSurfaceSpec *spec;
    if (role == SCREEN_KEYBOARD_ROLE)
    {
        spec = mir_connection_create_spec_for_input_method(d->connection, geometry.width(),
            geometry.height(), pixelFormat);
    }
    else
    {
        spec = mir_connection_create_spec_for_normal_surface(d->connection, geometry.width(),
            geometry.height(), pixelFormat);
    }
    mir_surface_spec_set_name(spec, title.data());

    // Create platform window
    mir_wait_for(mir_surface_create(spec, surfaceCreateCallback, this));
    mir_surface_spec_release(spec);

    DASSERT(d->surface != nullptr);
    d->eglSurface = eglCreateWindowSurface(eglDisplay, eglConfig,
        (EGLNativeWindowType)mir_buffer_stream_get_egl_native_window(mir_surface_get_buffer_stream(d->surface)), nullptr);
    DASSERT(d->eglSurface != EGL_NO_SURFACE);

    if (d->state == Qt::WindowFullScreen) {
    // TODO: We could set this on creation once surface spec supports it (mps already up)
        mir_wait_for(mir_surface_set_state(d->surface, mir_surface_state_fullscreen));
    }

    // Window manager can give us a final size different from what we asked for
    // so let's check what we ended up getting
    {
        MirSurfaceParameters parameters;
        mir_surface_get_parameters(d->surface, &parameters);

        geometry.setWidth(parameters.width);
        geometry.setHeight(parameters.height);
    }

    DLOG("[ubuntumirclient QPA] created surface has size (%d, %d)",
            geometry.width(), geometry.height());

    // Assume that the buffer size matches the surface size at creation time
    d->bufferSize = geometry.size();

    // Tell Qt about the geometry.
    QWindowSystemInterface::handleGeometryChange(window(), geometry);
    QPlatformWindow::setGeometry(geometry);
}

void UbuntuWindow::moveResize(const QRect& rect)
{
    (void) rect;
    // TODO: Not yet supported by mir.
}

void UbuntuWindow::handleSurfaceResize(int width, int height)
{
    QMutexLocker(&d->mutex);
    DLOG("UbuntuWindow::handleSurfaceResize(width=%d, height=%d) [%d]", width, height,
        d->frameNumber);

    // The current buffer size hasn't actually changed. so just render on it and swap
    // buffers in the hope that the next buffer will match the surface size advertised
    // in this event.
    // But since this event is processed by a thread different from the one that swaps
    // buffers, you can never know if this information is already outdated as there's
    // no synchronicity whatsoever between the processing of resize events and the
    // consumption of buffers.
    if (d->bufferSize.width() != width || d->bufferSize.height() != height) {
        // if the next buffer doesn't have a different size, try some
        // more
        // FIXME: This is working around a mir bug! We really shound't have to
        // swap more than once to get a buffer with the new size!
        d->resizeCatchUpAttempts = 2;

        QWindowSystemInterface::handleExposeEvent(window(), d->exposed ? QRect(QPoint(), geometry().size()) : QRect());
        QWindowSystemInterface::flushWindowSystemEvents();
    }
}

void UbuntuWindow::handleSurfaceFocusChange(bool focused)
{
    LOG("UbuntuWindow::handleSurfaceFocusChange(focused=%s)", focused ? "true" : "false");
    QWindow *activatedWindow = focused ? window() : nullptr;

    // System clipboard contents might have changed while this window was unfocused and wihtout
    // this process getting notified about it because it might have been suspended (due to
    // application lifecycle policies), thus unable to listen to any changes notified through
    // D-Bus.
    // Therefore let's ensure we are up to date with the system clipboard now that we are getting
    // focused again.
    if (focused) {
        d->clipboard->requestDBusClipboardContents();
    }

    QWindowSystemInterface::handleWindowActivated(activatedWindow, Qt::ActiveWindowFocusReason);
}

void UbuntuWindow::handleSurfaceExposeChange(bool exposed)
{
    QMutexLocker(&d->mutex);
    DLOG("UbuntuWindow::handleSurfaceExposeChange(exposed=%s)", exposed ? "true" : "false");

    if (d->exposed != exposed) {
        d->exposed = exposed;

        QWindowSystemInterface::handleExposeEvent(window(), d->exposed ? QRect(QPoint(), geometry().size()) : QRect());
        QWindowSystemInterface::flushWindowSystemEvents();
    }
}

void UbuntuWindow::setWindowState(Qt::WindowState state)
{
    QMutexLocker(&d->mutex);
    DLOG("UbuntuWindow::setWindowState (this=%p, %s)", this,  qtWindowStateToStr(state));

    if (state == d->state)
        return;

    // TODO: Perhaps we should check if the states are applied?
    mir_wait_for(mir_surface_set_state(d->surface, qtWindowStateToMirSurfaceState(state)));
    d->state = state;
}

void UbuntuWindow::setGeometry(const QRect& rect)
{
    DLOG("UbuntuWindow::setGeometry (this=%p)", this);

    bool doMoveResize;

    {
        QMutexLocker(&d->mutex);
        QPlatformWindow::setGeometry(rect);
        doMoveResize = d->state != Qt::WindowFullScreen && d->state != Qt::WindowMaximized;
    }

    if (doMoveResize) {
        moveResize(rect);
    }
}

void UbuntuWindow::setVisible(bool visible)
{
    QMutexLocker(&d->mutex);
    DLOG("UbuntuWindow::setVisible (this=%p, visible=%s)", this, visible ? "true" : "false");

    if (visible) {
        mir_wait_for(mir_surface_set_state(d->surface, qtWindowStateToMirSurfaceState(d->state)));
    } else {
        // TODO: Use the new mir_surface_state_hidden state instead of mir_surface_state_minimized.
        //       Will have to change qtmir and unity8 for that.
        mir_wait_for(mir_surface_set_state(d->surface, mir_surface_state_minimized));
    }

    QWindowSystemInterface::handleExposeEvent(window(), d->exposed ? QRect(QPoint(), geometry().size()) : QRect());
    QWindowSystemInterface::flushWindowSystemEvents();
}

bool UbuntuWindow::isExposed() const
{
    return d->exposed && window()->isVisible();
}

void* UbuntuWindow::eglSurface() const
{
    return d->eglSurface;
}

QSurfaceFormat UbuntuWindow::format() const
{
    return d->format;
}

WId UbuntuWindow::winId() const
{
    return d->id;
}

void UbuntuWindow::onBuffersSwapped_threadSafe(int newBufferWidth, int newBufferHeight)
{
    QMutexLocker(&d->mutex);

    bool sizeKnown = newBufferWidth > 0 && newBufferHeight > 0;

#if !defined(QT_NO_DEBUG)
    ++d->frameNumber;
#endif

    if (sizeKnown && (d->bufferSize.width() != newBufferWidth ||
                d->bufferSize.height() != newBufferHeight)) {
        d->resizeCatchUpAttempts = 0;

        DLOG("UbuntuWindow::onBuffersSwapped_threadSafe [%d] - buffer size changed from (%d,%d) to (%d,%d)"
               " resizeCatchUpAttempts=%d",
               d->frameNumber, d->bufferSize.width(), d->bufferSize.height(), newBufferWidth, newBufferHeight,
               d->resizeCatchUpAttempts);

        d->bufferSize.rwidth() = newBufferWidth;
        d->bufferSize.rheight() = newBufferHeight;

        QRect newGeometry;

        newGeometry = geometry();
        newGeometry.setWidth(d->bufferSize.width());
        newGeometry.setHeight(d->bufferSize.height());

        QPlatformWindow::setGeometry(newGeometry);
        QWindowSystemInterface::handleGeometryChange(window(), newGeometry, QRect());
    } else if (d->resizeCatchUpAttempts > 0) {
        --d->resizeCatchUpAttempts;
        DLOG("UbuntuWindow::onBuffersSwapped_threadSafe [%d] - buffer size (%d,%d). Redrawing to catch up a resized buffer."
               " resizeCatchUpAttempts=%d",
               d->frameNumber, d->bufferSize.width(), d->bufferSize.height(), d->resizeCatchUpAttempts);
        QWindowSystemInterface::handleExposeEvent(window(), d->exposed ? QRect(QPoint(), geometry().size()) : QRect());
    } else {
        DLOG("UbuntuWindow::onBuffersSwapped_threadSafe [%d] - buffer size (%d,%d). resizeCatchUpAttempts=%d",
               d->frameNumber, d->bufferSize.width(), d->bufferSize.height(), d->resizeCatchUpAttempts);
    }
}
