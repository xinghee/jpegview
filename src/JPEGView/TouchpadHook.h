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

// Pan and zoom are applied independently rather than being classified into one
// or the other. Finger separation and midpoint travel are physically separate
// quantities, so mirroring both is exactly what the hand is doing. That also
// removes every chance of a misclassification turning a drag into a zoom.
// OnPointer returns true when the message belongs to us.

// Ignore finger-separation noise below this (himetric = 0.01 mm).
#define TP_ZOOM_DEADBAND 6

// A gesture is classified once, after this much accumulated motion
// (himetric = 0.01 mm), and keeps that class until the last finger lifts.
#define TP_DECIDE_THRESHOLD 30

// Gesture classification is based on the angle between the two fingers'
// displacement vectors, which is what physically separates the two gestures:
//   drag  -> both fingers travel the SAME way      (cos ~ +1)
//   pinch -> fingers travel OPPOSITE ways          (cos ~ -1)
// Recorded sessions showed drag at +0.93..+1.00 and pinch at -0.87..-0.95,
// with nothing in between, so a plain cos < 0 test would already work.
#define TP_SAME_COS  0.3
#define TP_OPP_COS  (-0.3)

// A finger moving less than this (himetric) is treated as stationary and the
// frame is skipped - that also skips the half-updated frames where only one
// finger has been reported yet.
#define TP_MIN_FINGER_MOVE 4.0

#define GM_UNDECIDED 0
#define GM_PAN       1
#define GM_ZOOM      2

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
typedef BOOL (WINAPI *PFN_GetPointerDeviceRects)(HANDLE device, RECT* pointerDeviceRect, RECT* displayRect);

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
	int     lastDist;
	UINT32  frameId;       // last measured pointer frame
	int     gestureMode;   // GM_UNDECIDED / GM_PAN / GM_ZOOM, locked per gesture
	POINT   prevPts[2];    // per-finger position at the previous frame
	double  sumSame;       // accumulated motion while both fingers moved alike
	double  sumOpp;        // accumulated motion while both fingers moved apart
	int     msgCount;
	int     panCount;
	int     zoomCount;
	long    totalDx;
	long    totalDy;
	double  scale;      // pixels per himetric unit
	bool    scaleKnown;
	double  accX;       // sub-pixel remainder, without this slow drags never move
	double  accY;
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

inline PFN_GetPointerDeviceRects ResolveDeviceRects() {
	static PFN_GetPointerDeviceRects fn = NULL;
	static bool tried = false;
	if (!tried) {
		tried = true;
		HMODULE h = GetModuleHandleW(L"user32.dll");
		if (h != NULL) {
			fn = (PFN_GetPointerDeviceRects)GetProcAddress(h, "GetPointerDeviceRects");
		}
	}
	return fn;
}

// Touchpad coordinates are himetric (0.01 mm), so a raw delta is useless for
// moving pixels. GetPointerDeviceRects gives the touchpad size and the display
// size in the same units family; the ratio converts himetric to pixels.
// Falls back to a typical laptop ratio when the call is unavailable.
inline double ComputeScale(HANDLE device) {
	Ctx& c = C();
	if (c.scaleKnown) return c.scale;
	c.scale = 0.2;
	c.scaleKnown = true;

	PFN_GetPointerDeviceRects fn = ResolveDeviceRects();
	if (fn == NULL || device == NULL) {
		LogF("scale: fallback %.4f (GetPointerDeviceRects unavailable)", c.scale);
		return c.scale;
	}
	RECT dev;
	RECT scr;
	ZeroMemory(&dev, sizeof(dev));
	ZeroMemory(&scr, sizeof(scr));
	if (!fn(device, &dev, &scr)) {
		LogF("scale: fallback %.4f (GetPointerDeviceRects failed, err=%lu)",
			c.scale, (unsigned long)GetLastError());
		return c.scale;
	}
	double dw = (double)(dev.right - dev.left);
	double sw = (double)(scr.right - scr.left);
	if (dw > 0.0 && sw > 0.0) {
		c.scale = sw / dw;
		LogF("scale=%.5f px/himetric (device %dx%d, screen %dx%d)", c.scale,
			(int)dw, (int)(dev.bottom - dev.top), (int)sw, (int)(scr.bottom - scr.top));
	} else {
		LogF("scale: fallback %.4f (bad rects device=%dx%d screen=%dx%d)", c.scale,
			(int)dw, (int)(dev.bottom - dev.top), (int)sw, (int)(scr.bottom - scr.top));
	}
	return c.scale;
}

inline void ResetTracking() {
	Ctx& c = C();
	c.hasMid = false;
	c.lastMid.x = 0;
	c.lastMid.y = 0;
	c.accX = 0.0;
	c.accY = 0.0;
	c.frameId = 0;
	c.gestureMode = GM_UNDECIDED;
	c.sumSame = 0.0;
	c.sumOpp = 0.0;
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
			c.prevPts[i] = pt;
			c.activeCount++;
			ResetTracking();
			return;
		}
	}
	// No free slot: an earlier UP was lost. Drop the oldest contact rather than
	// ignoring the new one, which would leave the state machine wedged and make
	// every following gesture behave unpredictably.
	c.ids[0] = c.ids[1];
	c.pts[0] = c.pts[1];
	c.ids[1] = id;
	c.pts[1] = pt;
	ResetTracking();
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
			c.prevPts[i].x = 0;
			c.prevPts[i].y = 0;
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

// Returns true when the message belongs to us and must not reach DefWindowProc.
// Pan (midpoint travel) and zoom (finger spread) are independent physical
// quantities, so both are mirrored in the same frame instead of classifying the
// gesture into one or the other - that classification is what kept turning a
// drag into an accidental zoom.
inline bool OnPointer(UINT msg, WPARAM wParam, int& outDx, int& outDy, double& outZoom) {
	Ctx& c = C();
	outDx = 0;
	outDy = 0;
	outZoom = 1.0;
	if (!c.enabled) return false;

	PFN_GetPointerInfo gpi = ResolveGetPointerInfo();
	if (gpi == NULL) return false;

	UINT32 id = (UINT32)(wParam & 0xFFFF);   // equivalent of GET_POINTERID_WPARAM
	PI_LOCAL pi;
	ZeroMemory(&pi, sizeof(pi));
	if (!gpi(id, &pi)) return c.inGesture;

	// ptPixelLocation holds the mouse cursor position when the gesture started
	// and never changes during it - reading it yields a constant. The contact
	// position relative to the device lives in ptHimetricLocation (0.01 mm).
	POINT pt = pi.ptHimetricLocation;
	c.msgCount++;
	if (c.msgCount <= 400) {
		LogF("ptr msg=0x%04X id=%u type=%d him=(%d,%d) frame=%u flags=0x%08X",
			(unsigned)msg, (unsigned)id, (int)pi.pointerType, (int)pt.x, (int)pt.y,
			(unsigned)pi.frameId, (unsigned)pi.pointerFlags);
	}
	if (pi.pointerType != PT_TOUCHPAD) return false;

	if (msg == WM_POINTERDOWN) {
		// A lost UP leaves stale contacts behind; without clearing them the next
		// gesture inherits the leftovers and behaves erratically - works once,
		// then does nothing, then works again.
		if (c.activeCount >= 2) {
			c.ids[0] = 0;
			c.ids[1] = 0;
			c.activeCount = 0;
			ResetTracking();
			LogF("  down: cleared stale contacts");
		}
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

		// Frame sync.
		// WM_POINTERUPDATE carries ONE finger at a time. Computing the midpoint
		// right after each message therefore yields a snapshot that is half new
		// and half stale: the finger spread appears to jump by roughly half a
		// finger per message, which is easily large enough to look like a pinch
		// and hijack the gesture. Settling the PREVIOUS frame when a new frameId
		// arrives fixes it - at that moment every finger of that frame has been
		// written, so the snapshot is consistent. Costs one frame of latency.
		if (c.frameId != pi.frameId) {
			POINT mid;
			int dist = 0;
			if (GetMid(mid, dist)) {
				if (!c.hasMid) {
					c.lastMid = mid;
					c.lastDist = dist;
					c.hasMid = true;
				} else {
					int rawX = (int)mid.x - (int)c.lastMid.x;
					int rawY = (int)mid.y - (int)c.lastMid.y;
					int prevDist = c.lastDist;
					int dDist = dist - prevDist;
					c.lastMid = mid;
					c.lastDist = dist;

					// Where each finger moved this frame. Comparing the two
					// directions is what separates drag from pinch: dragging
					// sends both the same way, pinching sends them apart.
					double d0x = (double)(c.pts[0].x - c.prevPts[0].x);
					double d0y = (double)(c.pts[0].y - c.prevPts[0].y);
					double d1x = (double)(c.pts[1].x - c.prevPts[1].x);
					double d1y = (double)(c.pts[1].y - c.prevPts[1].y);
					c.prevPts[0] = c.pts[0];
					c.prevPts[1] = c.pts[1];
					double len0 = sqrt(d0x * d0x + d0y * d0y);
					double len1 = sqrt(d1x * d1x + d1y * d1y);

					if (c.activeCount >= 2 && len0 >= TP_MIN_FINGER_MOVE && len1 >= TP_MIN_FINGER_MOVE) {
						double dot = d0x * d1x + d0y * d1y;
						double cosv = dot / (len0 * len1);
						if (cosv > TP_SAME_COS) {
							c.sumSame += (len0 + len1) * 0.5;
						} else if (cosv < TP_OPP_COS) {
							c.sumOpp += (len0 < len1) ? len0 : len1;
						}
						if (c.gestureMode == GM_UNDECIDED && c.sumSame + c.sumOpp >= TP_DECIDE_THRESHOLD) {
							c.gestureMode = (c.sumOpp > c.sumSame) ? GM_ZOOM : GM_PAN;
							LogF("gesture=%s (same=%.0f opp=%.0f)",
								(c.gestureMode == GM_ZOOM) ? "zoom" : "pan", c.sumSame, c.sumOpp);
							c.accX = 0.0;
							c.accY = 0.0;
						}
					}

					// Wait for evidence before doing anything. Costs two or
					// three frames of latency, but avoids moving the image the
					// wrong way for those frames when the gesture turns out to
					// be a pinch.
					if (c.gestureMode == GM_UNDECIDED) return true;

					int pinch = (dDist < 0) ? -dDist : dDist;
					if (c.gestureMode == GM_ZOOM) {
						if (c.activeCount >= 2 && prevDist > 0 && pinch >= TP_ZOOM_DEADBAND) {
							double ratio = (double)dist / (double)prevDist;
							if (ratio < 0.92) ratio = 0.92;
							if (ratio > 1.08) ratio = 1.08;
							if (ratio != 1.0) {
								outZoom = ratio;
								c.zoomCount++;
								if (c.zoomCount <= 60) {
									LogF("  zoom dist %d->%d ratio=%.4f", prevDist, dist, ratio);
								}
							}
						}
					} else {
						if (rawX != 0 || rawY != 0) {
							double sc = ComputeScale(pi.sourceDevice);
							c.accX += (double)rawX * sc;
							c.accY += (double)rawY * sc;
							int px = (int)c.accX;
							int py = (int)c.accY;
							c.accX -= (double)px;
							c.accY -= (double)py;
							if (c.invertY) py = -py;
							if (px != 0 || py != 0) {
								outDx = px;
								outDy = py;
								c.panCount++;
								c.totalDx += px;
								c.totalDy += py;
								if (c.panCount <= 80) {
									LogF("  pan raw=(%d,%d) px=(%d,%d) scale=%.4f active=%d dist=%d",
										rawX, rawY, px, py, sc, c.activeCount, dist);
								}
							}
						}
					}
				}
			}
			c.frameId = pi.frameId;
		}

		UpdatePoint(id, pt);
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
	c.lastDist = 0;
	c.frameId = 0;
	c.gestureMode = GM_UNDECIDED;
	c.sumSame = 0.0;
	c.sumOpp = 0.0;
	c.prevPts[0].x = 0; c.prevPts[0].y = 0;
	c.prevPts[1].x = 0; c.prevPts[1].y = 0;
	c.msgCount = 0;
	c.panCount = 0;
	c.zoomCount = 0;
	c.totalDx = 0;
	c.totalDy = 0;
	c.scale = 0.0;
	c.scaleKnown = false;
	c.accX = 0.0;
	c.accY = 0.0;

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
