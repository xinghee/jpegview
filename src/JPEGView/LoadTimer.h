// LoadTimer.h
//
// Lightweight stage timing for image loading and first display. Compiled in but
// completely inert unless JPEGVIEW_LOAD_LOG=1 is set, so it can stay in the code
// permanently.
//
// Two reports are produced per opened image:
//
//   === photo.jpg                    <- CLoadTimer: inside the load thread
//     read            102.0 ms
//     decode         2118.5 ms
//     construct         1.7 ms
//     TOTAL          2222.2 ms
//
//   === photo.jpg  (open -> first frame)   <- CLoadTrace: cross-thread
//     load           2222.2 ms
//     4ch             120.0 ms
//     resample       3100.0 ms
//     lut              90.0 ms
//     TOTAL          5532.2 ms
//
// The second one answers "how long until the picture actually appears", which
// the per-function timer cannot see: decoding happens on the loader thread,
// resampling on the UI thread, and the gap between them is real user wait time.
//
// Output goes to <exe dir>-load.log, or to %TEMP% if that directory is not
// writable (Program Files is read-only for a normal user).

#pragma once

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

// Opens the shared log for appending; returns NULL when there is nowhere to
// write. Kept as a free function so both reporters use identical path rules.
static FILE* LoadLogOpen() {
	char path[MAX_PATH];
	path[0] = '\0';
	if (GetModuleFileNameA(NULL, path, MAX_PATH) == 0) return NULL;
	char* dot = strrchr(path, '.');
	if (dot != NULL) *dot = '\0';
	char full[MAX_PATH];
	full[0] = '\0';
	sprintf_s(full, MAX_PATH, "%s-load.log", path);

	FILE* f = NULL;
	if (fopen_s(&f, full, "a") == 0 && f != NULL) return f;

	char tmp[MAX_PATH];
	tmp[0] = '\0';
	if (GetTempPathA(MAX_PATH, tmp) == 0) return NULL;
	char alt[MAX_PATH];
	alt[0] = '\0';
	sprintf_s(alt, MAX_PATH, "%sJPEGView-load.log", tmp);
	if (fopen_s(&f, alt, "a") == 0 && f != NULL) return f;
	return NULL;
}

static bool LoadLogEnabled() {
	char buf[8];
	buf[0] = '\0';
	return GetEnvironmentVariableA("JPEGVIEW_LOAD_LOG", buf, (DWORD)sizeof(buf)) > 0 && buf[0] == '1';
}

static void LoadLogNarrow(LPCTSTR src, char* dst, int cch) {
	dst[0] = '\0';
	if (src == NULL) return;
#ifdef UNICODE
	WideCharToMultiByte(CP_ACP, 0, src, -1, dst, cch, NULL, NULL);
#else
	strncpy_s(dst, cch, src, _TRUNCATE);
#endif
}

// One-off line in the shared log, for stages that live in a different module
// and cannot reach the timers above. Inert unless logging is enabled.
static void LoadLogNote(const char* fmt, ...) {
	if (!LoadLogEnabled()) return;
	FILE* f = LoadLogOpen();
	if (f == NULL) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

// ---------------------------------------------------------------------------
// Cross-thread trace: open request -> first frame on screen.
// ---------------------------------------------------------------------------
class CLoadTrace {
public:
	static CLoadTrace& Inst() {
		static CLoadTrace instance;
		return instance;
	}

	bool Pending() const { return m_pending; }

	// Called when a read request starts. Returns true when this request owns the
	// trace, so its caller knows whether to report into it.
	//
	// A request that never gets painted leaves the trace pending, so a new
	// request takes it over after a while instead of blocking every later
	// report. While a trace is genuinely in flight (prefetch firing right after
	// the displayed image), it is kept - overwriting would lose the real one.
	bool Begin(LPCTSTR fileName) {
		if (!LoadLogEnabled()) return false;
		EnterCriticalSection(&m_cs);
		if (m_pending) {
			LARGE_INTEGER now;
			QueryPerformanceCounter(&now);
			if ((double)(now.QuadPart - m_start.QuadPart) * 1000.0 / (double)m_freq.QuadPart < 120000.0) {
				LeaveCriticalSection(&m_cs);
				return false;
			}
		}
		LoadLogNarrow(fileName, m_file, MAX_PATH);
		QueryPerformanceCounter(&m_start);
		m_last = m_start;
		m_count = 0;
		m_pending = true;
		LeaveCriticalSection(&m_cs);
		return true;
	}

	// Records the interval that just finished, labelled with tag.
	void Mark(const char* tag) {
		if (!m_pending) return;
		EnterCriticalSection(&m_cs);
		if (m_pending) MarkLocked(tag);
		LeaveCriticalSection(&m_cs);
	}

	// Records the last interval, writes the block and goes idle.
	void End() {
		if (!m_pending) return;
		EnterCriticalSection(&m_cs);
		if (m_pending) {
			m_pending = false;
			WriteBlockLocked();
		}
		LeaveCriticalSection(&m_cs);
	}

private:
	CLoadTrace() : m_pending(false), m_count(0) {
		InitializeCriticalSection(&m_cs);
		QueryPerformanceFrequency(&m_freq);
		m_start.QuadPart = 0;
		m_last.QuadPart = 0;
		m_file[0] = '\0';
	}
	~CLoadTrace() { DeleteCriticalSection(&m_cs); }
	CLoadTrace(const CLoadTrace&);
	CLoadTrace& operator=(const CLoadTrace&);

	static const int kMaxStages = 8;

	void MarkLocked(const char* tag) {
		if (m_count >= kMaxStages) return;
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		m_times[m_count] = (double)(now.QuadPart - m_last.QuadPart) * 1000.0 / (double)m_freq.QuadPart;
		strncpy_s(m_tags[m_count], tag, _TRUNCATE);
		m_count++;
		m_last = now;
	}

	void WriteBlockLocked() {
		// A repaint of an unrelated image can end a trace that never got a mark;
		// writing an empty block only adds noise.
		if (m_count == 0) return;
		FILE* f = LoadLogOpen();
		if (f == NULL) return;

		const char* base = strrchr(m_file, '\\');
		base = (base != NULL) ? base + 1 : m_file;

		double total = 0.0;
		for (int i = 0; i < m_count; i++) total += m_times[i];

		fprintf(f, "=== %s  (open -> first frame)\n", base);
		for (int i = 0; i < m_count; i++) {
			fprintf(f, "  %-12s %8.1f ms\n", m_tags[i], m_times[i]);
		}
		fprintf(f, "  %-12s %8.1f ms\n", "TOTAL", total);
		fclose(f);
	}

	CRITICAL_SECTION m_cs;
	LARGE_INTEGER m_freq;
	LARGE_INTEGER m_start;
	LARGE_INTEGER m_last;
	bool m_pending;
	char m_file[MAX_PATH];
	int m_count;
	char m_tags[kMaxStages][24];
	double m_times[kMaxStages];
};

// ---------------------------------------------------------------------------
// Per-function timer for the load thread itself.
// ---------------------------------------------------------------------------
class CLoadTimer {
public:
	explicit CLoadTimer(LPCTSTR fileName) : m_active(LoadLogEnabled()), m_traceOwner(false), m_count(0) {
		if (!m_active) return;
		QueryPerformanceFrequency(&m_freq);
		QueryPerformanceCounter(&m_start);
		m_last = m_start;
		LoadLogNarrow(fileName, m_file, MAX_PATH);
		// Start the cross-thread trace at the same moment, so its "load" stage
		// covers exactly what this timer measures. A request that did not get
		// to own the trace (a prefetch during the displayed image) must not
		// report into it.
		m_traceOwner = CLoadTrace::Inst().Begin(fileName);
	}

	void Mark(const char* tag) {
		if (!m_active || m_count >= kMaxStages) return;
		LARGE_INTEGER now;
		QueryPerformanceCounter(&now);
		m_times[m_count] = MsBetween(m_last, now);
		strncpy_s(m_tags[m_count], tag, _TRUNCATE);
		m_count++;
		m_last = now;
	}

	~CLoadTimer() {
		if (!m_active) return;
		// Hand the whole load interval to the trace as one stage; the trace
		// continues on the UI thread until the first frame is drawn.
		if (m_traceOwner) CLoadTrace::Inst().Mark("load");
		WriteReport();
	}

	static bool IsEnabled() { return LoadLogEnabled(); }

private:
	static const int kMaxStages = 8;

	double MsBetween(const LARGE_INTEGER& a, const LARGE_INTEGER& b) const {
		return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)m_freq.QuadPart;
	}

	void WriteReport() const {
		FILE* f = LoadLogOpen();
		if (f == NULL) return;

		const char* base = strrchr(m_file, '\\');
		base = (base != NULL) ? base + 1 : m_file;

		double total = 0.0;
		for (int i = 0; i < m_count; i++) total += m_times[i];

		fprintf(f, "=== %s\n", base);
		for (int i = 0; i < m_count; i++) {
			fprintf(f, "  %-12s %8.1f ms\n", m_tags[i], m_times[i]);
		}
		fprintf(f, "  %-12s %8.1f ms\n", "TOTAL", total);
		fclose(f);
	}

	bool m_active;
	bool m_traceOwner;
	LARGE_INTEGER m_freq;
	LARGE_INTEGER m_start;
	LARGE_INTEGER m_last;
	char m_file[MAX_PATH];
	int m_count;
	char m_tags[kMaxStages][24];
	double m_times[kMaxStages];
};
