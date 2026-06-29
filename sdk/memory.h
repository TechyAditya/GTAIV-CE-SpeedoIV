/*
 * sdk/memory.h - Safe memory read/probe helpers for 32-bit Windows processes
 *
 * Single responsibility: don't crash when poking at maybe-valid pointers.
 * Use the predicates before dereferencing.
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdint>

namespace sdk {

inline bool IsReadable(const void* addr, size_t len) {
    return !IsBadReadPtr(addr, len);
}

inline bool ReadPtr(uintptr_t addr, uintptr_t* out) {
    if (!IsReadable((void*)addr, 4)) return false;
    *out = *(uintptr_t*)addr;
    return true;
}

inline bool ReadFloat(uintptr_t addr, float* out) {
    if (!IsReadable((void*)addr, 4)) return false;
    *out = *(float*)addr;
    return true;
}

inline bool ReadByte(uintptr_t addr, uint8_t* out) {
    if (!IsReadable((void*)addr, 1)) return false;
    *out = *(uint8_t*)addr;
    return true;
}

/* Loose validity: any pointer outside the bottom 64KB and below the
 * user-mode ceiling on 32-bit Windows. */
inline bool IsValidPtr(uintptr_t p) { return p > 0x10000 && p < 0x7FFFFFFF; }

/* Stricter "is heap": excludes the executable image range, useful for
 * filtering globals vs allocated objects. */
inline bool IsHeapPtr(uintptr_t p) { return p >= 0x04000000 && p < 0x7FFFFFFF; }

} // namespace sdk
