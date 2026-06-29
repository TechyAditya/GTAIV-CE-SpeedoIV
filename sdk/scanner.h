/*
 * sdk/scanner.h - Pattern scanning over a memory range
 *
 * Mask convention: 'x' for a fixed byte, anything else (typically '?') for a
 * wildcard. The pattern array and the mask string must have the same length.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstring>
#include <cstdint>

namespace sdk {

inline uintptr_t FindPattern(uintptr_t base, size_t size,
                             const BYTE* pattern, const char* mask) {
    size_t plen = strlen(mask);
    if (size < plen) return 0;
    for (size_t i = 0; i <= size - plen; i++) {
        bool ok = true;
        for (size_t j = 0; j < plen && ok; j++) {
            if (mask[j] == 'x' && ((const BYTE*)(base + i))[j] != pattern[j])
                ok = false;
        }
        if (ok) return base + i;
    }
    return 0;
}

/* Scan that returns up to N matches by repeatedly calling FindPattern.
 * Caller-allocates `out`. Returns number of matches written (<= maxOut). */
inline int FindAll(uintptr_t base, size_t size,
                   const BYTE* pattern, const char* mask,
                   uintptr_t* out, int maxOut) {
    int n = 0;
    uintptr_t cur = base;
    size_t rem = size;
    while (n < maxOut) {
        uintptr_t hit = FindPattern(cur, rem, pattern, mask);
        if (!hit) break;
        out[n++] = hit;
        cur = hit + 1;
        if (cur >= base + size) break;
        rem = base + size - cur;
    }
    return n;
}

} // namespace sdk
