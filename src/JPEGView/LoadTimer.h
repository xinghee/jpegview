// LoadTimer.h
//
// Lightweight stage timing for image loading. Compiled in but completely inert
// unless JPEGVIEW_LOAD_LOG=1 is set, so it can stay in the code permanently.
//
// Usage: construct one object at the top of the load function, call Mark() at
// each stage boundary. The destructor writes the report, so every early return
// is covered without extra bookkeeping.
//
// Output goes to <exe dir>-load.log, or to %TEMP% if that directory is not
// writable (Program Files is read-only for a normal user).

#pragma once

#include <windows.h>
#include <stdio.h>
#include <string.h>

class CLoadTimer {
public:
	explicit CLoadTimer(const char* fileName) : m_active(IsEnabled()), m_count(0) {
		if (!m_active) return;
		QueryPerformanceFrequency(&m_freq);
		QueryPerformanceCounter(&m_start);
		m_last = m_start;
		strncpy_s(m_file, fileName != NULL ? fileName : "", _TRUNCATE);
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
		WriteReport();
	}

	static bool IsEnabled() {
		char buf[8];
		buf[0] = '\0';
		return GetEnvironmentVariableA("JPEGVIEW_LOAD_LOG", buf, (DWORD)sizeof(buf)) > 0 && buf[0] == '1';
	}

private:
	static const int kMaxStages = 8;

	double MsBetween(const LARGE_INTEGER& a, const LARGE_INTEGER& b) const {
		return (double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)m_freq.QuadPart;
	}

	void WriteReport() const {
		char path[MAX_PATH];
		path[0] = '\0';
		if (GetModuleFileNameA(NULL, path, MAX_PATH) == 0) return;
		char* dot = strrchr(path, '.');
		if (dot != NULL) *dot = '\0';
		char full[MAX_PATH];
		full[0] = '\0';
		sprintf_s(full, MAX_PATH, "%s-load.log", path);

		FILE* f = NULL;
		if (fopen_s(&f, full, "a") != 0 || f == NULL) {
			char tmp[MAX_PATH];
			tmp[0] = '\0';
			if (GetTempPathA(MAX_PATH, tmp) == 0) return;
			char alt[MAX_PATH];
			alt[0] = '\0';
			sprintf_s(alt, MAX_PATH, "%sJPEGView-load.log", tmp);
			if (fopen_s(&f, alt, "a") != 0 || f == NULL) return;
		}

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
	LARGE_INTEGER m_freq;
	LARGE_INTEGER m_start;
	LARGE_INTEGER m_last;
	char m_file[MAX_PATH];
	int m_count;
	char m_tags[kMaxStages][24];
	double m_times[kMaxStages];
};
