/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */
#pragma once

#ifdef __APPLE__

// On macOS the engine does not use Apple's native OpenGL.framework. SDL
// creates a plain Cocoa window; we drive OpenGL via Mesa + Zink + KosmicKrisp
// (Vulkan on Metal), wired up through EGL's `surfaceless` platform. The
// engine code keeps using the familiar `SDL_GL_*` call names — on Apple the
// macros at the bottom of this header redirect them to MacEGL equivalents.
//
// This header must be included AFTER <SDL.h>/<SDL_video.h> so that SDL's
// own `SDL_GL_*` function declarations are visible before the redirect
// macros take effect.

#include <SDL_video.h>

namespace MacEGL {

// Populate env vars that the Mesa/Zink/KosmicKrisp loader expects before any
// GL/Vulkan symbol is resolved. Safe to call multiple times.
void ConfigureEnvironment();

// SDL_GL_SetAttribute replacement: cache the requested attribute so CreateContext
// can consult it later (EGL doesn't have a process-global attribute bag).
int SetAttribute(SDL_GLattr attr, int value);

// SDL_GL_GetAttribute replacement — returns last cached value (or SDL fallback).
int GetAttribute(SDL_GLattr attr, int* value);

// SDL_GL_CreateContext replacement. Creates the EGL display, chooses a config,
// creates a context with the cached major/minor/profile/flags, creates a window
// surface on the SDL window's backing NSView layer, and makes it current.
// Returns an opaque non-null pointer on success so call sites can treat it
// like an SDL_GLContext; nullptr on failure.
SDL_GLContext CreateContext(SDL_Window* window);

// SDL_GL_DeleteContext replacement. Tears down the EGL context, surface, display.
void DeleteContext(SDL_GLContext ctx);

// SDL_GL_MakeCurrent replacement. Passing ctx=nullptr detaches the context.
int MakeCurrent(SDL_Window* window, SDL_GLContext ctx);

// SDL_GL_SwapWindow replacement.
void SwapWindow(SDL_Window* window);

// SDL_GL_SetSwapInterval replacement. Returns 0 on success, -1 on failure.
int SetSwapInterval(int interval);

// SDL_GL_GetSwapInterval replacement — returns last value we set.
int GetSwapInterval();

} // namespace MacEGL

// Redirect every SDL_GL_* call the engine makes. Keep call sites untouched.
#define SDL_GL_SetAttribute(attr, v)       MacEGL::SetAttribute(attr, v)
#define SDL_GL_GetAttribute(attr, vptr)    MacEGL::GetAttribute(attr, vptr)
#define SDL_GL_CreateContext(win)          MacEGL::CreateContext(win)
#define SDL_GL_DeleteContext(ctx)          MacEGL::DeleteContext(ctx)
#define SDL_GL_MakeCurrent(win, ctx)       MacEGL::MakeCurrent(win, ctx)
#define SDL_GL_SwapWindow(win)             MacEGL::SwapWindow(win)
#define SDL_GL_SetSwapInterval(i)          MacEGL::SetSwapInterval(i)
#define SDL_GL_GetSwapInterval()           MacEGL::GetSwapInterval()

#endif // __APPLE__
