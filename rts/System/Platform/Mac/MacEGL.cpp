/* This file is part of the Spring engine (GPL v2 or later), see LICENSE.html */
#ifdef __APPLE__

#include "MacEGL.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <array>
#include <cstdio>
#include <cstdlib>

#include "System/Log/ILog.h"

namespace {

EGLDisplay g_display = EGL_NO_DISPLAY;
EGLContext g_context = EGL_NO_CONTEXT;
EGLSurface g_surface = EGL_NO_SURFACE;
SDL_Window* g_window = nullptr;
void*      g_layer   = nullptr;
int g_lastDrawableW = 0;
int g_lastDrawableH = 0;
int g_swapInterval = 0;

// Cache for SDL_GL_SetAttribute values, indexed by SDL_GLattr.
std::array<int, 64> g_attribs{}; // SDL_GLattr enum is small; 64 is plenty
std::array<bool, 64> g_attribsSet{};

int GetAttrib(SDL_GLattr attr, int fallback) {
	const size_t idx = static_cast<size_t>(attr);
	if (idx < g_attribs.size() && g_attribsSet[idx])
		return g_attribs[idx];
	return fallback;
}

// Return the NSView's default backing layer (NSViewBackingLayer).
//
// This follows the lucamignatti gist path verbatim: Mesa's patched
// `surfaceless_metal_kopper_set_surface_create_info` detects that class
// name and swaps the layer for a fully-configured CAMetalLayer on the
// main thread, then hands the layer to `vkCreateMetalSurfaceEXT`.
//
// We still pump AppKit events first so the contentView has non-zero
// bounds by the time Mesa reads `[view bounds]` — otherwise the CAMetalLayer
// Mesa creates has 0x0 drawableSize and nothing you render is visible.
void* GetBackingLayer(SDL_Window* window) {
	SDL_ShowWindow(window);
	for (int i = 0; i < 8; ++i) SDL_PumpEvents();

	SDL_SysWMinfo wm;
	SDL_VERSION(&wm.version);
	if (!SDL_GetWindowWMInfo(window, &wm))
		return nullptr;

	void* nswindow = wm.info.cocoa.window;
	auto msgId   = reinterpret_cast<void*(*)(void*, SEL)>(objc_msgSend);
	auto msgBool = reinterpret_cast<void(*)(void*, SEL, BOOL)>(objc_msgSend);
	auto msgVoid = reinterpret_cast<void(*)(void*, SEL)>(objc_msgSend);

	void* view = msgId(nswindow, sel_registerName("contentView"));
	if (!view)
		return nullptr;
	msgBool(view, sel_registerName("setWantsLayer:"), YES);
	msgVoid(view, sel_registerName("layoutSubtreeIfNeeded"));
	return msgId(view, sel_registerName("layer"));
}

} // namespace

namespace MacEGL {

void ConfigureEnvironment() {
	if (!getenv("MESA_LOADER_DRIVER_OVERRIDE"))
		setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1);
	if (!getenv("EGL_PLATFORM"))
		setenv("EGL_PLATFORM", "surfaceless", 1);
}

int SetAttribute(SDL_GLattr attr, int value) {
	const size_t idx = static_cast<size_t>(attr);
	if (idx >= g_attribs.size())
		return -1;
	g_attribs[idx] = value;
	g_attribsSet[idx] = true;
	return 0;
}

int GetAttribute(SDL_GLattr attr, int* value) {
	if (!value)
		return -1;
	const size_t idx = static_cast<size_t>(attr);
	if (idx >= g_attribs.size() || !g_attribsSet[idx]) {
		*value = 0;
		return -1;
	}
	*value = g_attribs[idx];
	return 0;
}

SDL_GLContext CreateContext(SDL_Window* window) {
	ConfigureEnvironment();

	g_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (g_display == EGL_NO_DISPLAY) {
		LOG_L(L_FATAL, "[MacEGL] eglGetDisplay returned EGL_NO_DISPLAY");
		return nullptr;
	}

	EGLint eglMajor = 0, eglMinor = 0;
	if (!eglInitialize(g_display, &eglMajor, &eglMinor)) {
		LOG_L(L_FATAL, "[MacEGL] eglInitialize failed (0x%x)", eglGetError());
		return nullptr;
	}
	LOG("[MacEGL] EGL %d.%d  vendor=%s  version=%s",
	    eglMajor, eglMinor,
	    eglQueryString(g_display, EGL_VENDOR),
	    eglQueryString(g_display, EGL_VERSION));

	if (!eglBindAPI(EGL_OPENGL_API)) {
		LOG_L(L_FATAL, "[MacEGL] eglBindAPI(EGL_OPENGL_API) failed (0x%x)", eglGetError());
		return nullptr;
	}

	const int red   = GetAttrib(SDL_GL_RED_SIZE,   8);
	const int green = GetAttrib(SDL_GL_GREEN_SIZE, 8);
	const int blue  = GetAttrib(SDL_GL_BLUE_SIZE,  8);
	const int depth = GetAttrib(SDL_GL_DEPTH_SIZE, 24);
	const int stencil = GetAttrib(SDL_GL_STENCIL_SIZE, 8);
	// Request ALPHA=8 to line up with CAMetalLayer's BGRA8Unorm (the layer
	// is always 4-channel; asking for an RGB-only EGL config can make Zink
	// pick a 3-channel swapchain format that KosmicKrisp rejects).
	EGLint cfgAttribs[] = {
		EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
		EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
		EGL_RED_SIZE,   red,
		EGL_GREEN_SIZE, green,
		EGL_BLUE_SIZE,  blue,
		EGL_ALPHA_SIZE, 8,
		EGL_DEPTH_SIZE, depth,
		EGL_STENCIL_SIZE, stencil,
		EGL_NONE
	};
	// Enumerate all matching configs and pick one whose R/G/B/A are *exactly*
	// 8 bits. eglChooseConfig treats the RED_SIZE/etc. attributes as minimums,
	// so on Zink it will happily return a 16-bit-per-channel config which
	// propagates all the way into the swapchain image format — KosmicKrisp's
	// Metal WSI only advertises BGRA8* / R16G16B16A16_SFLOAT / 10A2 formats,
	// and CAMetalLayer refuses any 16-bit UNORM format, so a 16-bit config
	// ends up with a cascading type mismatch between the 16-bit logical
	// image and the 8-bit Metal drawable.
	const EGLint kMaxConfigs = 64;
	EGLConfig allConfigs[kMaxConfigs];
	EGLint nConfigs = 0;
	if (!eglChooseConfig(g_display, cfgAttribs, allConfigs, kMaxConfigs, &nConfigs) || nConfigs < 1) {
		LOG_L(L_FATAL, "[MacEGL] eglChooseConfig failed (0x%x)", eglGetError());
		return nullptr;
	}

	EGLConfig cfg = nullptr;
	for (EGLint i = 0; i < nConfigs; ++i) {
		EGLint r=0, g=0, b=0, a=0, d=0, s=0;
		eglGetConfigAttrib(g_display, allConfigs[i], EGL_RED_SIZE,   &r);
		eglGetConfigAttrib(g_display, allConfigs[i], EGL_GREEN_SIZE, &g);
		eglGetConfigAttrib(g_display, allConfigs[i], EGL_BLUE_SIZE,  &b);
		eglGetConfigAttrib(g_display, allConfigs[i], EGL_ALPHA_SIZE, &a);
		eglGetConfigAttrib(g_display, allConfigs[i], EGL_DEPTH_SIZE, &d);
		eglGetConfigAttrib(g_display, allConfigs[i], EGL_STENCIL_SIZE, &s);
		if (r == 8 && g == 8 && b == 8 && a == 8) {
			cfg = allConfigs[i];
			LOG("[MacEGL] picked EGL config #%d: RGBA=%d%d%d%d D=%d S=%d",
			    i, r, g, b, a, d, s);
			break;
		}
	}
	if (!cfg) {
		LOG_L(L_WARNING, "[MacEGL] no 8-bit-per-channel EGL config available, falling back to first match");
		cfg = allConfigs[0];
	}

	const int major = GetAttrib(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	const int minor = GetAttrib(SDL_GL_CONTEXT_MINOR_VERSION, 0);
	const int profileMask = GetAttrib(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_COMPATIBILITY);
	const int contextFlags = GetAttrib(SDL_GL_CONTEXT_FLAGS, 0);

	const EGLint profileBit =
		(profileMask == SDL_GL_CONTEXT_PROFILE_CORE)
			? EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT
			: EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT;

	EGLint eglFlags = 0;
	if (contextFlags & SDL_GL_CONTEXT_DEBUG_FLAG)
		eglFlags |= EGL_CONTEXT_OPENGL_DEBUG_BIT_KHR;

	EGLint ctxAttribs[] = {
		EGL_CONTEXT_MAJOR_VERSION, major,
		EGL_CONTEXT_MINOR_VERSION, minor,
		EGL_CONTEXT_OPENGL_PROFILE_MASK, profileBit,
		EGL_CONTEXT_FLAGS_KHR, eglFlags,
		EGL_NONE
	};
	g_context = eglCreateContext(g_display, cfg, EGL_NO_CONTEXT, ctxAttribs);
	if (g_context == EGL_NO_CONTEXT) {
		LOG_L(L_FATAL, "[MacEGL] eglCreateContext(%d.%d, %s) failed (0x%x)",
		      major, minor,
		      (profileMask == SDL_GL_CONTEXT_PROFILE_CORE) ? "core" : "compat",
		      eglGetError());
		return nullptr;
	}

	void* backingLayer = GetBackingLayer(window);
	if (!backingLayer) {
		LOG_L(L_FATAL, "[MacEGL] Could not obtain backing layer from SDL window");
		return nullptr;
	}

	g_surface = eglCreateWindowSurface(g_display, cfg,
	                                   reinterpret_cast<EGLNativeWindowType>(backingLayer),
	                                   nullptr);
	if (g_surface == EGL_NO_SURFACE) {
		LOG_L(L_FATAL, "[MacEGL] eglCreateWindowSurface failed (0x%x)", eglGetError());
		return nullptr;
	}

	// Stash the SDL window so SwapWindow can detect view-resize events and
	// push the new size into the CAMetalLayer's drawableSize (otherwise the
	// layer keeps its initial-bounds size after SDL_SetWindowSize and Metal
	// end up stretching a stale texture each frame — ~1 FPS in fullscreen).
	g_window = window;
	g_lastDrawableW = 0;
	g_lastDrawableH = 0;

	if (!eglMakeCurrent(g_display, g_surface, g_surface, g_context)) {
		LOG_L(L_FATAL, "[MacEGL] eglMakeCurrent failed (0x%x)", eglGetError());
		return nullptr;
	}

	// Return opaque non-null sentinel so callers can compare against nullptr.
	return reinterpret_cast<SDL_GLContext>(0x1);
}

void DeleteContext(SDL_GLContext /*ctx*/) {
	if (g_display == EGL_NO_DISPLAY)
		return;
	eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if (g_surface != EGL_NO_SURFACE) {
		eglDestroySurface(g_display, g_surface);
		g_surface = EGL_NO_SURFACE;
	}
	if (g_context != EGL_NO_CONTEXT) {
		eglDestroyContext(g_display, g_context);
		g_context = EGL_NO_CONTEXT;
	}
	eglTerminate(g_display);
	g_display = EGL_NO_DISPLAY;
}

int MakeCurrent(SDL_Window* /*window*/, SDL_GLContext ctx) {
	if (g_display == EGL_NO_DISPLAY)
		return -1;
	if (ctx == nullptr) {
		return eglMakeCurrent(g_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) ? 0 : -1;
	}
	return eglMakeCurrent(g_display, g_surface, g_surface, g_context) ? 0 : -1;
}

// Re-query the NSView's current CAMetalLayer and push the SDL window size
// into `setDrawableSize:`. Safe to call every frame — Metal no-ops the call
// when the size hasn't changed; the objc_msgSend overhead is a handful of ns.
static void SyncDrawableSize() {
	if (g_window == nullptr)
		return;

	int w = 0, h = 0;
	SDL_GetWindowSize(g_window, &w, &h);
	if (w <= 0 || h <= 0)
		return;
	if (w == g_lastDrawableW && h == g_lastDrawableH)
		return;

	SDL_SysWMinfo wm;
	SDL_VERSION(&wm.version);
	if (!SDL_GetWindowWMInfo(g_window, &wm))
		return;

	auto msgId = reinterpret_cast<void*(*)(void*, SEL)>(objc_msgSend);
	void* nswindow = wm.info.cocoa.window;
	void* view = msgId(nswindow, sel_registerName("contentView"));
	if (!view)
		return;
	void* layer = msgId(view, sel_registerName("layer"));
	if (!layer)
		return;

	// [layer setDrawableSize:(CGSize){w, h}] — CGSize is two CGFloat (double
	// on arm64), matching our Mesa patch's existing size struct.
	struct CGSizeD { double w, h; };
	auto msgSendSize = reinterpret_cast<void(*)(void*, SEL, CGSizeD)>(objc_msgSend);
	msgSendSize(layer, sel_registerName("setDrawableSize:"), CGSizeD{double(w), double(h)});

	g_lastDrawableW = w;
	g_lastDrawableH = h;
}

void SwapWindow(SDL_Window* /*window*/) {
	if (g_display != EGL_NO_DISPLAY && g_surface != EGL_NO_SURFACE) {
		SyncDrawableSize();
		eglSwapBuffers(g_display, g_surface);
	}
}

int SetSwapInterval(int interval) {
	if (g_display == EGL_NO_DISPLAY)
		return -1;
	if (!eglSwapInterval(g_display, interval))
		return -1;
	g_swapInterval = interval;
	return 0;
}

int GetSwapInterval() {
	return g_swapInterval;
}

} // namespace MacEGL

#endif // __APPLE__
