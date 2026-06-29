/*
 * sdk/logging.h - Lazy-open file logger with optional Debug gating
 *
 * Behavior modes (set via SetMode before any Log call):
 *
 *   Debug=true  -> log file is truncated/created on LogOpen, every call writes.
 *   Debug=false -> log file is NOT touched on LogOpen. The first warn/error
 *                  call (LogWarn / LogError) lazy-opens the file in APPEND mode,
 *                  preserving any history from previous sessions. Info-level
 *                  Log() calls are dropped to disk but still keep the recent
 *                  ones in an in-memory ring buffer that is flushed on first
 *                  warn/error so the context surrounding the issue isn't lost.
 *
 * Format: `[HH:MM:SS.mmm] message`.
 *
 * Reusable across any GTA IV / Windows ASI plugin.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>

namespace sdk {

enum LogLevel { LL_INFO = 0, LL_WARN = 1, LL_ERROR = 2 };

namespace log_detail {
    inline FILE*       g_file       = nullptr;
    inline char        g_path[MAX_PATH] = {0};
    inline bool        g_pathSet    = false;
    inline bool        g_debug      = false;   /* Debug=true forces verbose */
    inline bool        g_opened     = false;   /* file is open right now */
    inline bool        g_truncated  = false;   /* did we already truncate this run */

    /* Ring buffer for INFO messages when Debug=false. Flushed to disk on first
     * warn/error to provide context. */
    static const int   RING_CAP = 64;
    static const int   RING_LINE = 256;
    inline char        g_ring[RING_CAP][RING_LINE];
    inline int         g_ringHead = 0;
    inline int         g_ringCount = 0;

    inline void Ensure() {
        if (g_opened || !g_pathSet) return;
        const char* mode = (g_debug && !g_truncated) ? "w" : "a";
        g_file = fopen(g_path, mode);
        g_opened = (g_file != nullptr);
        if (g_opened && g_debug) g_truncated = true;
    }

    inline void FlushRing() {
        if (!g_opened || g_ringCount == 0) return;
        int start = (g_ringCount < RING_CAP) ? 0 : g_ringHead;
        int n = g_ringCount < RING_CAP ? g_ringCount : RING_CAP;
        if (n > 0) fputs("--- buffered context ---\n", g_file);
        for (int i = 0; i < n; i++) {
            int idx = (start + i) % RING_CAP;
            fputs(g_ring[idx], g_file);
        }
        if (n > 0) fputs("--- end context ---\n", g_file);
        g_ringHead = 0;
        g_ringCount = 0;
    }

    inline void RingPush(const char* line) {
        char* slot = g_ring[g_ringHead];
        size_t n = strlen(line);
        if (n >= RING_LINE) n = RING_LINE - 1;
        memcpy(slot, line, n);
        slot[n] = 0;
        g_ringHead = (g_ringHead + 1) % RING_CAP;
        if (g_ringCount < RING_CAP * 2) g_ringCount++;
    }
}

/* Set up logging mode + target filename. The actual file is opened lazily
 * (immediately if Debug=true, on first warn/error otherwise). */
inline void LogOpen(const char* filename, bool debug = false) {
    char dir[MAX_PATH];
    GetModuleFileNameA(NULL, dir, MAX_PATH);
    char* sl = strrchr(dir, '\\'); if (sl) *sl = 0;
    snprintf(log_detail::g_path, MAX_PATH, "%s\\%s", dir, filename);
    log_detail::g_pathSet = true;
    log_detail::g_debug   = debug;
    if (debug) {
        log_detail::Ensure();   /* truncates the file */
    }
}

/* Update debug mode after LogOpen. If switching to debug=true and the file
 * hasn't been truncated yet, reopen in truncate mode. */
inline void SetDebug(bool debug) {
    log_detail::g_debug = debug;
    if (debug && log_detail::g_pathSet && !log_detail::g_truncated) {
        if (log_detail::g_file) { fclose(log_detail::g_file); log_detail::g_file = nullptr; log_detail::g_opened = false; }
        log_detail::Ensure();
    }
}

inline bool IsDebug() { return log_detail::g_debug; }

inline void LogClose() {
    if (log_detail::g_file) { fclose(log_detail::g_file); log_detail::g_file = nullptr; }
    log_detail::g_opened = false;
}

inline void LogAt(LogLevel lvl, const char* fmt, ...) {
    /* Format line into a stack buffer with timestamp + level prefix */
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[log_detail::RING_LINE];
    int n = snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s",
                     st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                     lvl == LL_WARN ? "WARN: " : (lvl == LL_ERROR ? "ERROR: " : ""));
    if (n < 0 || n >= (int)sizeof(line)) return;

    va_list ap; va_start(ap, fmt);
    int n2 = vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
    va_end(ap);
    if (n2 < 0) return;
    int total = n + n2;
    if (total >= (int)sizeof(line) - 1) total = sizeof(line) - 2;
    line[total] = '\n';
    line[total + 1] = 0;

    if (log_detail::g_debug) {
        log_detail::Ensure();
        if (log_detail::g_file) {
            fputs(line, log_detail::g_file);
            fflush(log_detail::g_file);
        }
        return;
    }

    /* Debug=false path */
    if (lvl == LL_INFO) {
        /* Buffer info in ring until/unless something warn-or-worse happens */
        log_detail::RingPush(line);
        return;
    }
    /* warn/error: lazy-open, flush context, write line */
    log_detail::Ensure();
    if (log_detail::g_file) {
        log_detail::FlushRing();
        fputs(line, log_detail::g_file);
        fflush(log_detail::g_file);
    }
}

/* Convenience wrappers preserving the original Log() signature */
inline void Log(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char tmp[log_detail::RING_LINE];
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    LogAt(LL_INFO, "%s", tmp);
}

inline void LogWarn(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char tmp[log_detail::RING_LINE];
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    LogAt(LL_WARN, "%s", tmp);
}

inline void LogError(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char tmp[log_detail::RING_LINE];
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    LogAt(LL_ERROR, "%s", tmp);
}

} // namespace sdk
