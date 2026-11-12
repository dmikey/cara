// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
namespace Cara {
enum class SessionPresentation { Stopped, Loading, Running, Paused, Stopping, Failed };
constexpr SessionPresentation ResolveSessionPresentation(bool opening, bool running, bool loaded,
                                                        bool stopping, bool paused, bool failed) {
    if (failed) return SessionPresentation::Failed;
    if (opening) return SessionPresentation::Loading;
    if (!running) return SessionPresentation::Stopped;
    if (stopping) return SessionPresentation::Stopping;
    if (!loaded) return SessionPresentation::Loading;
    return paused ? SessionPresentation::Paused : SessionPresentation::Running;
}
constexpr const char* SessionPresentationLabel(SessionPresentation state) {
    switch (state) {
    case SessionPresentation::Loading: return "Loading…";
    case SessionPresentation::Running: return "Running";
    case SessionPresentation::Paused: return "Paused";
    case SessionPresentation::Stopping: return "Stopping…";
    case SessionPresentation::Failed: return "Error";
    default: return "Stopped";
    }
}
constexpr bool ClearSessionPresentation(SessionPresentation state) {
    return state != SessionPresentation::Running && state != SessionPresentation::Paused;
}
constexpr bool AcceptSessionFrame(SessionPresentation state, unsigned session, unsigned current) {
    return session == current && state == SessionPresentation::Running;
}
}
