#define _USE_MATH_DEFINES // for M_PI on MSVC

#include "streaming/session.h"

#include <Limelight.h>
#include "SDL_compat.h"

#include <QRandomGenerator>

#include <cmath>
#include <thread>

#ifdef Q_OS_WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

#define AUTO_MOUSE_TICK_MS 8
#define AUTO_MOUSE_OVERLAY_MS 2000
#define AUTO_MOUSE_EDGE_MARGIN 20
#define AUTO_MOUSE_WINDOW_MARGIN 40

static const char* k_AutoMouseOnText = "Auto mouse: ON";
static const char* k_AutoMouseOffText = "Auto mouse: OFF";

static double randRange(double lo, double hi)
{
    return lo + QRandomGenerator::global()->generateDouble() * (hi - lo);
}

static void playToggleSound(bool on)
{
#ifdef Q_OS_WIN32
    // Beep() blocks for the duration of the tone, so keep it off the SDL main thread
    std::thread([on]() {
        Beep(on ? 1320 : 880, 70);
    }).detach();
#else
    Q_UNUSED(on);
#endif
}

void SdlInputHandler::toggleAutoMouse()
{
    m_AutoMouseEnabled = !m_AutoMouseEnabled;
    m_AutoMouseMoving = false;
    m_AutoMouseRemX = m_AutoMouseRemY = 0;
    m_AutoMouseNextMoveTime = SDL_GetTicks() + 300;

    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                "Auto mouse %s",
                m_AutoMouseEnabled ? "enabled" : "disabled");

    playToggleSound(m_AutoMouseEnabled);

    // We re-use the status update overlay and hide it again from the tick handler
    Session::get()->getOverlayManager().updateOverlayText(Overlay::OverlayStatusUpdate,
                                                          m_AutoMouseEnabled ? k_AutoMouseOnText : k_AutoMouseOffText);
    Session::get()->getOverlayManager().setOverlayState(Overlay::OverlayStatusUpdate, true);
    m_AutoMouseOverlayVisible = true;
    m_AutoMouseOverlayHideTime = SDL_GetTicks() + AUTO_MOUSE_OVERLAY_MS;

    // The timer keeps running after disabling until the overlay has been hidden
    if (m_AutoMouseTimer == 0) {
        m_AutoMouseTimer = SDL_AddTimer(AUTO_MOUSE_TICK_MS, autoMouseTimerCallback, this);
    }
}

Uint32 SdlInputHandler::autoMouseTimerCallback(Uint32 interval, void* param)
{
    auto me = reinterpret_cast<SdlInputHandler*>(param);

    // Don't flood the event queue if the main thread is busy
    if (!me->m_AutoMouseTickPending.exchange(true)) {
        SDL_Event event;
        event.type = SDL_USEREVENT;
        event.user.code = SDL_CODE_AUTOMOUSE_TICK;
        event.user.data1 = nullptr;
        event.user.data2 = nullptr;
        if (SDL_PushEvent(&event) <= 0) {
            me->m_AutoMouseTickPending = false;
        }
    }

    return interval;
}

void SdlInputHandler::getWindowScreenRect(SDL_Rect* rect, int margin)
{
    int x, y, w, h;
    int top = 0, left = 0, bottom = 0, right = 0;

    SDL_GetWindowPosition(m_Window, &x, &y);
    SDL_GetWindowSize(m_Window, &w, &h);

    // Include the title bar and borders, since the cursor over those is still over our window
    SDL_GetWindowBordersSize(m_Window, &top, &left, &bottom, &right);

    rect->x = x - left - margin;
    rect->y = y - top - margin;
    rect->w = w + left + right + margin * 2;
    rect->h = h + top + bottom + margin * 2;
}

bool SdlInputHandler::isCursorOverWindow(int globalX, int globalY)
{
    // While SDL has the cursor locked to our focused window, it's by definition ours. Don't use
    // isCaptureActive() here: in absolute mouse mode it stays true even after the cursor
    // leaves the window or the window loses focus.
    if (SDL_GetRelativeMouseMode() && (SDL_GetWindowFlags(m_Window) & SDL_WINDOW_INPUT_FOCUS)) {
        return true;
    }

    if (SDL_GetWindowFlags(m_Window) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) {
        return false;
    }

    SDL_Rect rect;
    getWindowScreenRect(&rect, 0);
    SDL_Point point = { globalX, globalY };
    return SDL_PointInRect(&point, &rect);
}

void SdlInputHandler::getAutoMousePoint(double progress, double* x, double* y)
{
    // A straight line plus a sideways bulge that peaks mid-way, so the path is a gentle arc
    double bulge = sin(M_PI * progress);
    *x = m_AutoMousePath.startX + (m_AutoMousePath.endX - m_AutoMousePath.startX) * progress + m_AutoMousePath.bowX * bulge;
    *y = m_AutoMousePath.startY + (m_AutoMousePath.endY - m_AutoMousePath.startY) * progress + m_AutoMousePath.bowY * bulge;
}

bool SdlInputHandler::planAutoMouseMove(int globalX, int globalY, Uint32 now)
{
    // Stay on the display the cursor is currently on
    SDL_Rect bounds = {};
    bool foundDisplay = false;
    for (int i = 0; i < SDL_GetNumVideoDisplays(); i++) {
        SDL_Rect displayBounds;
        SDL_Point point = { globalX, globalY };
        if (SDL_GetDisplayBounds(i, &displayBounds) == 0 && SDL_PointInRect(&point, &displayBounds)) {
            bounds = displayBounds;
            foundDisplay = true;
            break;
        }
    }
    if (!foundDisplay) {
        return false;
    }

    SDL_Rect avoidRect;
    getWindowScreenRect(&avoidRect, AUTO_MOUSE_WINDOW_MARGIN);
    bool avoidWindow = !(SDL_GetWindowFlags(m_Window) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN));

    for (int attempt = 0; attempt < 16; attempt++) {
        // Mostly short hops like a person fidgeting, sometimes a long sweep across the screen
        double dist = randRange(0, 1) < 0.7 ? randRange(60, 380) :
                                              randRange(300, 0.7 * qMax(bounds.w, bounds.h));
        double angle = randRange(0, 2 * M_PI);

        double endX = qBound<double>(bounds.x + AUTO_MOUSE_EDGE_MARGIN,
                                     globalX + cos(angle) * dist,
                                     bounds.x + bounds.w - AUTO_MOUSE_EDGE_MARGIN - 1);
        double endY = qBound<double>(bounds.y + AUTO_MOUSE_EDGE_MARGIN,
                                     globalY + sin(angle) * dist,
                                     bounds.y + bounds.h - AUTO_MOUSE_EDGE_MARGIN - 1);

        double dx = endX - globalX;
        double dy = endY - globalY;
        double len = hypot(dx, dy);
        if (len < 40) {
            continue;
        }

        double bow = randRange(-0.12, 0.12) * len;

        m_AutoMousePath.startX = globalX;
        m_AutoMousePath.startY = globalY;
        m_AutoMousePath.endX = endX;
        m_AutoMousePath.endY = endY;
        m_AutoMousePath.bowX = -dy / len * bow;
        m_AutoMousePath.bowY = dx / len * bow;

        // Don't plan a path that would run through our own window
        bool crossesWindow = false;
        if (avoidWindow) {
            for (int k = 0; k <= 24 && !crossesWindow; k++) {
                double px, py;
                getAutoMousePoint(k / 24.0, &px, &py);
                SDL_Point point = { (int)px, (int)py };
                crossesWindow = SDL_PointInRect(&point, &avoidRect);
            }
        }
        if (crossesWindow) {
            continue;
        }

        // Fitts's law-like timing: longer movements take longer, but not proportionally
        m_AutoMousePath.duration = (250 + 180 * log2(1 + len / 40)) * randRange(0.85, 1.25);
        m_AutoMousePath.startTime = now;
        m_AutoMousePath.lastX = globalX;
        m_AutoMousePath.lastY = globalY;
        return true;
    }

    return false;
}

void SdlInputHandler::autoMouseTick()
{
    m_AutoMouseTickPending = false;

    Uint32 now = SDL_GetTicks();

    if (m_AutoMouseOverlayVisible && SDL_TICKS_PASSED(now, m_AutoMouseOverlayHideTime)) {
        // Only hide the overlay if something else hasn't taken it over in the meantime
        const char* text = Session::get()->getOverlayManager().getOverlayText(Overlay::OverlayStatusUpdate);
        if (strcmp(text, k_AutoMouseOnText) == 0 || strcmp(text, k_AutoMouseOffText) == 0) {
            Session::get()->getOverlayManager().setOverlayState(Overlay::OverlayStatusUpdate, false);
        }
        m_AutoMouseOverlayVisible = false;
    }

    if (!m_AutoMouseEnabled) {
        if (!m_AutoMouseOverlayVisible && m_AutoMouseTimer != 0) {
            SDL_RemoveTimer(m_AutoMouseTimer);
            m_AutoMouseTimer = 0;
        }
        return;
    }

    int globalX, globalY;
    SDL_GetGlobalMouseState(&globalX, &globalY);

    if (isCursorOverWindow(globalX, globalY)) {
        // The user is controlling the remote PC, so get out of the way. We'll
        // start again shortly after the cursor leaves the window.
        m_AutoMouseMoving = false;
        m_AutoMouseRemX = m_AutoMouseRemY = 0;
        m_AutoMouseNextMoveTime = now + 400;
        return;
    }

    if (!m_AutoMouseMoving) {
        if (!SDL_TICKS_PASSED(now, m_AutoMouseNextMoveTime)) {
            return;
        }

        if (!planAutoMouseMove(globalX, globalY, now)) {
            m_AutoMouseNextMoveTime = now + 1000;
            return;
        }

        m_AutoMouseMoving = true;
    }

    double t = qMin((now - m_AutoMousePath.startTime) / m_AutoMousePath.duration, 1.0);

    // Minimum-jerk profile: smooth acceleration, a peak mid-way, and a soft landing
    double eased = t * t * t * (10 - 15 * t + 6 * t * t);

    double pathX, pathY;
    getAutoMousePoint(eased, &pathX, &pathY);

    // Apply only the change since the last tick relative to where the cursor is now.
    // That way the user's own real mouse movement and ours add up instead of fighting.
    m_AutoMouseRemX += pathX - m_AutoMousePath.lastX;
    m_AutoMouseRemY += pathY - m_AutoMousePath.lastY;
    m_AutoMousePath.lastX = pathX;
    m_AutoMousePath.lastY = pathY;

    int stepX = (int)lround(m_AutoMouseRemX);
    int stepY = (int)lround(m_AutoMouseRemY);
    if (stepX != 0 || stepY != 0) {
        m_AutoMouseRemX -= stepX;
        m_AutoMouseRemY -= stepY;
        SDL_WarpMouseGlobal(globalX + stepX, globalY + stepY);
    }

    if (t >= 1.0) {
        m_AutoMouseMoving = false;

        // Humans rest between movements for a variable amount of time
        m_AutoMouseNextMoveTime = now + (Uint32)(randRange(0, 1) < 0.15 ? randRange(2000, 5000) :
                                                                         randRange(250, 1800));
    }
}
