// TouchpadHook.h
//
// Two-finger touchpad pan support for JPEGView.
//
// Background
// ----------
// Windows normally converts touchpad two-finger gestures into wheel messages
// before the application ever sees them. That is exactly why a two-finger swipe
// zooms the picture instead of panning it: from the app's point of view it is
// indistinguishable from a mouse wheel.
//
// RegisterTouchpadCapableWindow() asks the system to deliver the raw WM_POINTER
// messages for two-finger gestures to our window instead. Anything not consumed
// is forwarded to DefWindowProc, and the system then converts it back into a
// wheel message - so a failure to handle degrades to the original behaviour
// rather than breaking input. Ordinary mouse messages are never affected.
//
// Consumption rule: a gesture is either fully consumed or fully forwarded.
// Mixing the two would confuse the system's pointer-to-wheel state machine,
// so once the first PT_TOUCHPAD contact is accepted, the whole sequence is
// ours until the last contact lifts.
//
// Why nothing here comes from the SDK headers
// -------------------------------------------
// stdafx.h sets _WIN32_WINNT to 0x0501 (Windows XP), so winuser.h does not
// declare POINTER_INFO, GetPointerInfo or RegisterTouchpadCapableWindow at all.
// Everything is therefore declared locally and resolved with GetProcAddress.
// That also removes any import-library dependency, which keeps the linker happy
// regardless of SDK version.
//
// Header-only on purpose: no new .cpp has to be registered in the project file.

#pragma once

#include <windows.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

namespace TouchpadHook {

// ---- constants that old SDK headers do not define ----
// POINTER_INPUT_TYPE per winuser.h: PT_POINTER=1, PT_TOUCH=2, PT_PEN=3,
// PT_MOUSE=4, PT_TOUCHPAD=5. Getting this wrong makes every gesture look
// foreign and get forwarded back as a wheel message.
#ifndef PT_TOUCHPAD
#define PT_TOUCHPAD 5
#endif
#ifndef WM_POINTERUPDATE
#define WM_POINTERUPDATE 0x0245
#endif
#ifndef WM_POINTERDOWN
#define WM_POINTERDOWN 0x0246
#endif
#ifndef WM_POINTERUP
#define WM_POINTERUP 0x0247
#endif
#ifndef WM_POINTERCAPTURECHANGED
#define WM_POINTERCAPTURECHANGED 0x024C
#endif

// ---- local mirror of POINTER_INFO ----
// Field order and types must match winuser.h exactly: GetPointerInfo writes
// into this buffer, so a layout mismatch would corrupt the stack.
struct PI_LOCAL {
	INT32   pointerType;
	UINT32  pointerId;
	UINT32  frameId;
	DWORD   pointerFlags;
	HANDLE  sourceDevice;
	HWND    hwndTarget;
	POINT   ptPixelLocation;
	POINT   ptHimetricLocation;
	POINT   ptPixelLocationRaw;
	POINT   ptHimetricLocationRaw;
	DWORD   dwTime;
	UINT32  historyCount;
	INT32   InputState;
	DWORD   dwKeyStates;
	UINT64  PerformanceCount;
	INT32   ButtonChangeType;
};

typedef BOOL (WINAPI *PFN_GetPointerInfo)(UINT32 pointerId, PVOID pointerInfo);
typedef BOOL (WINAPI *PFN_RegisterTouchpadCapableWindow)(HWND hWnd, BOOL fEnable);

// RegisterTouchpadCapableWindow is exported by ordinal only on Windows 11.
#define TP_ORDINAL_REGISTER_WINDOW 2689

struct Ctx {
	HWND    hwnd;
	FILE*   log;
	bool    apiFound;
	bool    gpiFound;
	bool    registered;
	bool    enabled;
	bool    invertY;
	bool    hasMid;
	bool    inGesture;
	int     activeCount;
	UINT32  ids[2];
	POINT   pts[2];
	POINT   lastMid;
	int     msgCount;
	int     panCount;
	long    totalDx;
	long    totalDy;
};

inline Ctx& C() {
	static Ctx c = {};
	return c;
}

inline void LogF(const char* fmt, ...) {
	Ctx& c = C();
	if (c.log == NULL) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(c.log, fmt, ap);
	va_end(ap);
	fputc('\n', c.log);
	fflush(c.log);
}

inline void OpenLog() {
	Ctx& c = C();
	if (c.log != NULL) return;
	char path[MAX_PATH];
	path[0] = '\0';
	if (GetModuleFileNameA(NULL, path, MAX_PATH) == 0) return;
	char* dot = strrchr(path, '.');
	if (dot != NULL) *dot = '\0';
	char full[MAX_PATH];
	full[0] = '\0';
	sprintf_s(full, MAX_PATH, "%s-touchpad.log", path);
	if (fopen_s(&c.log, full, "w") != 0) {
		c.log = NULL;
	}
}

inline PFN_RegisterTouchpadCapableWindow ResolveRegisterFn() {
	static PFN_RegisterTouchpadCapableWindow fn = NULL;
	static bool tried = false;
	if (!tried) {
		tried = true;
		HMODULE h = GetModuleHandleW(L"user32.dll");
		if (h != NULL) {
			fn = (PFN_RegisterTouchpadCapableWindow)GetProcAddress(h, MAKEINTRESOURCEA(TP_ORDINAL_REGISTER_WINDOW));
			if (fn == NULL) {
				fn = (PFN_RegisterTouchpadCapableWindow)GetProcAddress(h, "RegisterTouchpadCapableWindow");
			}
		}
	}
	return fn;
}

inline PFN_GetPointerInfo ResolveGetPointerInfo() {
	static PFN_GetPointerInfo fn = NULL;
	static bool tried = false;
	if (!tried) {
		tried = true;
		HMODULE h = GetModuleHandleW(L"user32.dll");
		if (h != NULL) {
			fn = (PFN_GetPointerInfo)GetProcAddress(h, "GetPointerInfo");
		}
	}
	return fn;
}

inline void ResetTracking() {
	Ctx& c = C();
	c.hasMid = false;
	c.lastMid.x = 0;
	c.lastMid.y = 0;
}

inline void AddPoint(UINT32 id, POINT pt) {
	Ctx& c = C();
	for (int i = 0; i < 2; i++) {
		if (c.ids[i] == id) {
			c.pts[i] = pt;
			return;
		}
	}
	for (int i = 0; i < 2; i++) {
		if (c.ids[i] == 0) {
			c.ids[i] = id;
			c.pts[i] = pt;
			c.activeCount++;
			ResetTracking();
			return;
		}
	}
}

inline void UpdatePoint(UINT32 id, POINT pt) {
	Ctx& c = C();
	for (int i = 0; i < 2; i++) {
		if (c.ids[i] == id) {
			c.pts[i] = pt;
			return;
		}
	}
}

inline void RemovePoint(UINT32 id) {
	Ctx& c = C();
	for (int i = 0; i < 2; i++) {
		if (c.ids[i] == id) {
			c.ids[i] = 0;
			c.pts[i].x = 0;
			c.pts[i].y = 0;
			if (c.activeCount > 0) c.activeCount--;
			ResetTracking();
			if (c.activeCount == 0) c.inGesture = false;
			return;
		}
	}
}

inline bool GetMid(POINT& mid, int& dist) {
	Ctx& c = C();
	if (c.activeCount <= 0) return false;
	if (c.activeCount == 1) {
		for (int i = 0; i < 2; i++) {
			if (c.ids[i] != 0) {
				mid = c.pts[i];
				dist = 0;
				return true;
			}
		}
		return false;
	}
	mid.x = (c.pts[0].x + c.pts[1].x) / 2;
	mid.y = (c.pts[0].y + c.pts[1].y) / 2;
	double dx = (double)(c.pts[0].x - c.pts[1].x);
	double dy = (double)(c.pts[0].y - c.pts[1].y);
	dist = (int)sqrt(dx * dx + dy * dy);
	return true;
}

// Returns true when the message belongs to us and must NOT reach DefWindowProc.
// outDx/outDy receive the pan delta in pixels (may be 0).
inline bool OnPointer(UINT msg, WPARAM wParam, int& outDx, int& outDy) {
	Ctx& c = C();
	outDx = 0;
	outDy = 0;
	if (!c.enabled) return false;

	PFN_GetPointerInfo gpi = ResolveGetPointerInfo();
	if (gpi == NULL) return false;

	UINT32 id = (UINT32)(wParam & 0xFFFF);   // equivalent of GET_POINTERID_WPARAM
	PI_LOCAL pi;
	ZeroMemory(&pi, sizeof(pi));
	if (!gpi(id, &pi)) return c.inGesture;

	POINT pt = pi.ptPixelLocation;
	c.msgCount++;
	// Log BEFORE the type check - otherwise a wrong pointer type silently
	// disables the whole hook with no trace in the log at all.
	if (c.msgCount <= 400) {
		LogF("ptr msg=0x%04X id=%u type=%d pos=(%d,%d) flags=0x%08X",
			(unsigned)msg, (unsigned)id, (int)pi.pointerType, (int)pt.x, (int)pt.y, (unsigned)pi.pointerFlags);
	}
	if (pi.pointerType != PT_TOUCHPAD) return false;

	if (msg == WM_POINTERDOWN) {
		AddPoint(id, pt);
		c.inGesture = true;
		return true;
	}

	if (msg == WM_POINTERUP || msg == WM_POINTERCAPTURECHANGED) {
		RemovePoint(id);
		return true;
	}

	if (msg == WM_POINTERUPDATE) {
		if (!c.inGesture) return false;
		UpdatePoint(id, pt);
		POINT mid;
		int dist = 0;
		if (!GetMid(mid, dist)) return true;
		if (!c.hasMid) {
			c.lastMid = mid;
			c.hasMid = true;
			return true;
		}
		int dx = (int)mid.x - (int)c.lastMid.x;
		int dy = (int)mid.y - (int)c.lastMid.y;
		c.lastMid = mid;
		if (dx == 0 && dy == 0) return true;

		if (c.invertY) dy = -dy;
		outDx = dx;
		outDy = dy;
		c.panCount++;
		c.totalDx += dx;
		c.totalDy += dy;
		if (c.panCount <= 80) {
			LogF("  pan dx=%d dy=%d (mid=%d,%d active=%d dist=%d)", dx, dy,
				(int)mid.x, (int)mid.y, c.activeCount, dist);
		}
		return true;
	}

	return c.inGesture;
}

// Call once, after the main window exists.
inline void Init(HWND hwnd) {
	Ctx& c = C();
	c.hwnd = hwnd;
	c.enabled = false;
	c.apiFound = false;
	c.gpiFound = false;
	c.registered = false;
	c.inGesture = false;
	c.activeCount = 0;
	c.ids[0] = 0;
	c.ids[1] = 0;
	c.hasMid = false;
	c.msgCount = 0;
	c.panCount = 0;
	c.totalDx = 0;
	c.totalDy = 0;

	OpenLog();
	if (c.log == NULL) return;
	LogF("=== JPEGView touchpad hook ===");
	LogF("sizeof(PI_LOCAL)=%d (host x64 expects 96)", (int)sizeof(PI_LOCAL));

	char env[16];
	env[0] = '\0';
	if (GetEnvironmentVariableA("JPEGVIEW_NO_TOUCHPAD", env, sizeof(env)) > 0 && env[0] == '1') {
		LogF("disabled by JPEGVIEW_NO_TOUCHPAD=1");
		return;
	}
	env[0] = '\0';
	if (GetEnvironmentVariableA("JPEGVIEW_TOUCHPAD_INVERT_Y", env, sizeof(env)) > 0 && env[0] == '1') {
		c.invertY = true;
		LogF("invert Y enabled");
	}

	PFN_GetPointerInfo gpi = ResolveGetPointerInfo();
	c.gpiFound = (gpi != NULL);
	LogF("GetPointerInfo: %s", c.gpiFound ? "resolved" : "NOT FOUND");

	PFN_RegisterTouchpadCapableWindow fn = ResolveRegisterFn();
	c.apiFound = (fn != NULL);
	LogF("RegisterTouchpadCapableWindow: %s", c.apiFound ? "resolved" : "NOT FOUND (pre-Win11 system?)");
	if (fn == NULL || gpi == NULL) {
		LogF("hook inactive - touchpad keeps using the default wheel translation");
		return;
	}

	SetLastError(0);
	BOOL ok = fn(hwnd, TRUE);
	DWORD err = GetLastError();
	c.registered = (ok != FALSE);
	LogF("register(hwnd=%p, TRUE) -> %s, GetLastError=%lu",
		(void*)hwnd, c.registered ? "OK" : "FAILED", (unsigned long)err);
	c.enabled = c.registered;
	if (c.registered) {
		LogF("hook active - two-finger gestures now arrive as WM_POINTER");
	}
}

inline void Shutdown() {
	Ctx& c = C();
	if (c.log != NULL) {
		LogF("=== summary: msgs=%d pans=%d totalDx=%ld totalDy=%ld ===",
			c.msgCount, c.panCount, c.totalDx, c.totalDy);
		fclose(c.log);
		c.log = NULL;
	}
}

} // namespace TouchpadHook
