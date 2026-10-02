// code_check.h - "is the game's code at this address what this mod was written for?"
//
// The mods hook and patch ed8.exe at fixed addresses. Before a mod touches any of them it compares
// the bytes there with the ones it was written against, and refuses (log_error, nothing installed)
// on a mismatch: another game build (ed8jp.exe, a game patch, an exe an old SenPatcher patched on
// disk) or another patcher that changed the same code must cost a feature, never crash the game.
//
// SenPatcher (v1.x, DINPUT8.dll) patches the exe in memory before any mod loads (its DllMain runs
// before the game's entry point; the loader's mods load after that). None of its v1.3.1 patch sites
// overlaps a site the mods use; where a later version does (the CS1 camera sensitivity patch at
// 0x0053c874), the mod accepts SenPatcher's bytes explicitly - InjectedJumpTarget() below reads
// SenPatcher's InjectJumpIntoCode<N> shape: a 5-byte jmp rel32, padded to N bytes with int3/nop.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace atmt_code {

// One expected byte run: `size` bytes of `bytes` at `address`; `what` names it in the log.
struct Expect {
    uintptr_t address;
    const char* bytes;
    unsigned size;
    const char* what;
};

// The whole range is committed and readable (no guard page).
inline bool Readable(uintptr_t address, size_t size) {
    uintptr_t at = address;
    const uintptr_t end = address + size;
    while (at < end) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<const void*>(at), &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0 || mbi.Protect == 0) return false;
        at = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    }
    return true;
}

inline bool Matches(uintptr_t address, const void* bytes, size_t size) {
    return Readable(address, size) && std::memcmp(reinterpret_cast<const void*>(address), bytes, size) == 0;
}

// "55 8b ec ..." of what is at `address` now (or "unreadable").
inline void Hex(uintptr_t address, size_t size, char* out, size_t out_size) {
    if (out_size == 0) return;
    out[0] = '\0';
    if (!Readable(address, size)) {
        std::snprintf(out, out_size, "unreadable");
        return;
    }
    size_t used = 0;
    for (size_t i = 0; i < size && used + 4 < out_size; ++i) {
        used += static_cast<size_t>(std::snprintf(out + used, out_size - used, i == 0 ? "%02x" : " %02x",
                                                  reinterpret_cast<const uint8_t*>(address)[i]));
    }
}

// Every entry matches. Otherwise false, and `why` says which one and what is there instead.
inline bool CheckAll(const Expect* table, size_t count, char* why, size_t why_size) {
    for (size_t i = 0; i < count; ++i) {
        const Expect& e = table[i];
        if (Matches(e.address, e.bytes, e.size)) continue;
        if (why != nullptr && why_size != 0) {
            char found[3 * 16 + 1];
            Hex(e.address, e.size < 16 ? e.size : 16, found, sizeof(found));
            std::snprintf(why, why_size,
                          "%s at 0x%08x is [%s], not the code this mod was written for (another game "
                          "build, or another patch changed it)",
                          e.what, static_cast<unsigned>(e.address), found);
        }
        return false;
    }
    return true;
}

template <size_t N>
inline bool CheckAll(const Expect (&table)[N], char* why, size_t why_size) {
    return CheckAll(table, N, why, why_size);
}

// When `address` holds a jmp rel32 padded to `length` bytes with int3 or nop (SenPatcher's
// InjectJumpIntoCode<length>, or this toolkit's own splice), its target if that is readable; else 0.
inline uintptr_t InjectedJumpTarget(uintptr_t address, size_t length) {
    if (length < 5 || !Readable(address, length)) return 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(address);
    if (p[0] != 0xe9) return 0;
    for (size_t i = 5; i < length; ++i) {
        if (p[i] != 0xcc && p[i] != 0x90) return 0;
    }
    int32_t rel = 0;
    std::memcpy(&rel, p + 1, sizeof(rel));
    const uintptr_t target = address + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
    return Readable(target, 16) ? target : 0;
}

}  // namespace atmt_code
