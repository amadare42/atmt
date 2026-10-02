// addr_hook.cpp - the one hook this mod needs.
//
// The game's own "set the message's run" call (0x00701E50 in the shipped ed8.exe) is a
// thiscall with the run pointer as its argument. Everything the logger needs is in that one
// frame, and all of it is the game's own data:
//
//   ecx         -> the message state; +0x358 is the name plate the game is about to draw
//   [esp+4]     -> the run pointer; the text is at run+5 (the stream stores a run as
//                  `11 <u32>` followed by the bytes)
//
// So: no searching, no matching, no catalog, nothing remembered. The speaker is read from
// that object (again at write time, in case the game sets the plate a moment later), the
// text straight from the run.
#include "atmt.h"
#include "code_check.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace atmt {

namespace {

volatile LONG g_in_probe = 0;
// The settings are OWNED here, by value: the caller's Config is very often a local (it was,
// in the thread that loads the mod), so keeping a pointer to it left g_config dangling once
// that thread exited - the first message then crashed the game dereferencing it.
Config g_config_storage;
const Config* g_config = &g_config_storage;
LogSink* g_sink = nullptr;
// The address the hook sits on (= the loader's handle for it), kept so a handover can take this
// build's hook away again before another build installs its own (see UninstallAddressHooks).
void* g_hook_target = nullptr;

// The same run is set again while a box is measured and redrawn: log it once per window.
//
// Keyed on the run pointer *and* its content, not the pointer alone. The game reuses run
// buffers across genuinely different lines (measured: of 32,354 logged lines only 21,742
// distinct addresses appeared, and the busiest hosted 8-9 different texts). A pointer-only key with a
// 10 second window silently ate any later line - including a repeated line like "..." - that
// landed on a recently used address, which is how lines were intermittently missing from the
// log while everything around them worked. A measure/redraw pass resolves within a handful of
// frames, so the window only needs to be that long, not a conversation's worth of reading time.
constexpr int kRecentRuns = 4;
constexpr DWORD kRecentWindowMs = 1000;
struct RecentRun {
    uint32_t run = 0;
    uint32_t hash = 0;
    DWORD ms = 0;
};
RecentRun g_recent[kRecentRuns];
int g_recent_pos = 0;

// Pages after a `02 03` break are logged together with the message's first page (the setter
// is never called for them - see NextPage). Should the game ever hand one of them to the
// setter after all, it is already in the log: these are remembered for long enough to cover
// reading a whole message.
constexpr int kAheadPages = 16;
constexpr DWORD kAheadWindowMs = 120000;
RecentRun g_ahead[kAheadPages];
int g_ahead_pos = 0;

// The later pages of a message are read at the setter call but *held* until the game shows them:
// logging them right away put lines in the panel the player had not reached yet. The game's own
// renderer (FUN_005ef810, called by the box's draw at 0x703410 and its measure at 0x7035c0/0x706fa0)
// reads the message state's
//   +0x32C  the run pointer the setter stored (offsets below are relative to it)
//   +0x330  how far into it the text has been typed out
//   +0x334  where the page on screen starts
// so a page is on screen once the page start has moved past the `02 03` before it, or the typing
// has got into the page's own text. Either is enough - whichever of the two the game moves - and
// both are monotonic within one message, so the pages come out in order. When +0x32C is no longer
// the run (the next message was set, or the box was reset), whatever is still held goes out: the
// game has moved past those pages, and dropping them would be the old "missing lines" bug again.
struct HeldPage {
    uintptr_t state = 0;     // the message state (the setter's ecx)
    uintptr_t base = 0;      // the run pointer the setter stored at +0x32C
    uint32_t brk = 0;        // offset of the `02` that ends the page before this one
    uint32_t text_at = 0;    // offset of this page's first text byte
    int page = 0;
    // The probe runs *before* the game's setter stores +0x32C, so for a moment the old pointer is
    // still there: a mismatch only means "message gone" once the run was seen in place (or after
    // kHeldSettleMs, in case it never is).
    bool seen = false;
    DWORD held_ms = 0;
    CatalogEntry entry;
    uintptr_t address = 0;
};
constexpr DWORD kHeldSettleMs = 2000;
std::deque<HeldPage> g_held;
CRITICAL_SECTION g_held_lock;
volatile LONG g_held_ready = 0;

uint32_t HashBytes(const std::string& s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) {
        h ^= c;
        h *= 16777619u;
    }
    return h;
}

bool Readable(const void* p, size_t n) {
    if (p == nullptr) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const uint8_t* start = static_cast<const uint8_t*>(p);
    const uint8_t* limit = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
    return start + n <= limit;
}

bool ReadByteAt(uintptr_t p, uint8_t* out) {
    if (p < 0x10000 || !Readable(reinterpret_cast<const void*>(p), 1)) return false;
    *out = *reinterpret_cast<const uint8_t*>(p);
    return true;
}

// Where the run's bytes start, relative to the pointer the game passed.
//
// The game hands over two shapes, and the game's own data says which: the byte at the pointer
// decides, so nothing has to be guessed.
//
//   `11 <u32>` <bytes>   the text opcode - the bytes start 5 in
//                        measured: 11 18 11 00 00 then "Ahh... Um... It looks like"
//   <bytes>              a run the game has already advanced into
//                        measured: after the name header 10 00 00 1a 03 04 then "Oh, darling..."
//
// A run can never start with 0x11 (its bytes are printable, or start with a '#' code), so the
// test is exact rather than a heuristic. The name-header signature is handled too: it is
// unambiguous (10 00 00 1a) and costs nothing, in case a caller ever passes one.
unsigned RunTextOffset(uintptr_t run, unsigned opcode_operand_bytes) {
    uint8_t first = 0;
    if (!ReadByteAt(run, &first)) return 0;
    if (first == 0x11) return opcode_operand_bytes;   // the text opcode: 1 byte + its operand
    if (first == 0x10) {
        uint8_t b[4];
        for (unsigned i = 0; i < 4; ++i) {
            if (!ReadByteAt(run + i, &b[i])) return 0;
        }
        if (b[1] == 0x00 && b[2] == 0x00 && b[3] == 0x1A) return 6;   // 10 00 00 1a <u16>
    }
    return 0;
}

// A bounded, printable, NUL-terminated string from the game (used for diagnostics: the name
// plate field of the message state).
std::string ReadAt(uintptr_t p, unsigned max_len) {
    std::string out;
    if (p < 0x10000) return out;
    for (unsigned i = 0; i < max_len; ++i) {
        if (i % 32 == 0 && !Readable(reinterpret_cast<const void*>(p + i), 1)) break;
        const char c = *reinterpret_cast<const char*>(p + i);
        if (c == '\0') break;
        if (static_cast<unsigned char>(c) < 0x20) break;
        out.push_back(c);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// The first bytes at an address, so a diagnostic can show why something was not recognised.
std::string HexAt(uintptr_t p, unsigned count) {
    std::string out;
    for (unsigned i = 0; i < count; ++i) {
        uint8_t b = 0;
        if (!ReadByteAt(p + i, &b)) break;
        char buf[4];
        _snprintf(buf, sizeof(buf), "%02x ", b);
        out += buf;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// The bytes of one text run, exactly as the game stores them: runs end at a control byte,
// and 0x01 is the break inside a message (kept, so `raw` stays byte-for-byte). `end`, when
// given, receives the address of the control byte that ended the run.
bool ReadRun(uintptr_t ptr, std::string* raw, std::string* text, uintptr_t* end = nullptr) {
    const int kMax = 1024;
    if (ptr < 0x10000 || !Readable(reinterpret_cast<const void*>(ptr), 1)) return false;
    std::string bytes;
    uintptr_t p = ptr;
    bool ended = false;
    for (int i = 0; i < kMax; ++i, ++p) {
        // checked at the start and at every page boundary, so a run that ends a region stops
        // there instead of faulting
        if ((i == 0 || (p & 0xFFF) == 0) && !Readable(reinterpret_cast<const void*>(p), 1)) break;
        const uint8_t b = *reinterpret_cast<const uint8_t*>(p);
        if (b == 0x01) {
            bytes.push_back('\x01');
            continue;
        }
        if (b < 0x20) {   // 0x00 ends the run, 0x02 ends the page
            ended = true;
            break;
        }
        bytes.push_back(static_cast<char>(b));
    }
    if (end != nullptr) *end = ended ? p : 0;
    if (bytes.empty()) return false;
    *raw = bytes;
    std::string flattened = bytes;
    for (char& c : flattened) {
        if (c == '\x01') c = '\n';
    }
    *text = StripCodes(flattened);
    return !text->empty();
}

// Where the next page of the same message starts, or 0 when the message ends at `end`.
//
// The setter is only called for a message's *first* page: the game keeps the whole message
// behind that one pointer (+0x32C) and walks the pages itself, so a page after a `02 03`
// break never passes through the hook. That is what made lines go missing - measured in
// t1010.dat, where every page after a break was absent from the log while its first page
// was there:
//
//   "...You are not" 01 "my friend." 02 03 | 11 ff 20 00 00 | "We may be in the same group..."
//   "...Mmm..."                      02 03 | 11 d7 14 00 00 | "#E[A]#M0Well, let's go."
//
// `02 00` (or any other byte after the 02) ends the message. After `02 03` the page may carry
// its own header before the text; the ones the scripts use are handled, anything else ends
// the walk rather than guessing:
//   11 <u32> / 12 <u32>   the text opcode (the operand is the voice line)
//   10 00 00 1a <u16>     a name header
//   0b                    a one-byte code (t0060.dat: "...unconfirmed." 02 03 0b "This ancient")
uintptr_t NextPage(uintptr_t end) {
    uint8_t b = 0;
    if (end == 0 || !ReadByteAt(end, &b) || b != 0x02) return 0;
    if (!ReadByteAt(end + 1, &b) || b != 0x03) return 0;
    uintptr_t p = end + 2;
    for (int guard = 0; guard < 4; ++guard) {
        if (!ReadByteAt(p, &b)) return 0;
        if (b >= 0x20 || b == 0x01) return p;   // the text itself
        if (b == 0x11 || b == 0x12) {
            p += 5;
        } else if (b == 0x10) {
            uint8_t h[3];
            for (unsigned i = 0; i < 3; ++i) {
                if (!ReadByteAt(p + 1 + i, &h[i])) return 0;
            }
            if (h[0] != 0x00 || h[1] != 0x00 || h[2] != 0x1A) return 0;
            p += 6;
        } else if (b == 0x0B) {
            p += 1;
        } else {
            return 0;
        }
    }
    return 0;
}

bool ReadU32At(uintptr_t p, uint32_t* out) {
    if (p < 0x10000 || !Readable(reinterpret_cast<const void*>(p), 4)) return false;
    *out = *reinterpret_cast<const volatile uint32_t*>(p);
    return true;
}

// Logs the held pages the game has reached (see HeldPage). `force_state` releases everything held
// for that state regardless (its next message is being set); `all` releases everything (shutdown,
// handover). Called on the game's thread from the probe and on the flush thread, so it locks - and
// writes while locked, which is what keeps two callers from interleaving one message's pages.
void ReleaseHeld(uintptr_t force_state, bool all) {
    if (InterlockedCompareExchange(&g_held_ready, 0, 0) == 0 || g_sink == nullptr) return;
    EnterCriticalSection(&g_held_lock);
    std::vector<uintptr_t> blocked;   // states whose next page is not on screen yet: their later ones wait too
    for (auto it = g_held.begin(); it != g_held.end();) {
        HeldPage& h = *it;
        const char* why = nullptr;
        uint32_t cur = 0, typed = 0, start = 0;
        const bool readable = ReadU32At(h.state + 0x32C, &cur) && ReadU32At(h.state + 0x330, &typed)
                              && ReadU32At(h.state + 0x334, &start);
        const bool in_place = readable && cur == static_cast<uint32_t>(h.base);
        if (in_place) h.seen = true;
        const bool settled = h.seen || (GetTickCount() - h.held_ms) >= kHeldSettleMs;
        if (all) why = "released";
        else if (h.state == force_state) why = "next message";
        else if (std::find(blocked.begin(), blocked.end(), h.state) != blocked.end()) why = nullptr;
        else if (!in_place) why = settled ? "message gone" : nullptr;
        else if (start > h.brk) why = "page start";
        else if (typed > h.text_at) why = "typed";
        if (why == nullptr) {
            blocked.push_back(h.state);
            ++it;
            continue;
        }
        g_sink->LineFromObject(h.entry, h.state, g_config->message_plate_offset, h.address,
                               "message");
        if (g_config->diagnostics) {
            char msg[256];
            _snprintf(msg, sizeof(msg),
                      "message state=0x%08x run=0x%08x page %d shown (%s: +0x32C=0x%08x "
                      "+0x330=%u +0x334=%u, break at %u, text at %u)",
                      static_cast<unsigned>(h.state), static_cast<unsigned>(h.base), h.page, why,
                      cur, typed, start, h.brk, h.text_at);
            g_sink->Diagnostic(msg);
        }
        it = g_held.erase(it);
    }
    LeaveCriticalSection(&g_held_lock);
}

void Probe(void* snapshot) {
    if (InterlockedCompareExchange(&g_in_probe, 1, 0) != 0) return;
    struct Guard {
        ~Guard() { InterlockedExchange(&g_in_probe, 0); }
    } guard;

    uintptr_t* snap = static_cast<uintptr_t*>(snapshot);
    const uintptr_t ecx = snap[1 + 6];                 // the message state (thiscall)
    const uintptr_t original_esp = snap[1 + 3] + 4;    // + the pushed eflags
    uintptr_t run = 0;
    if (Readable(reinterpret_cast<const void*>(original_esp + 4), 4)) {
        run = *reinterpret_cast<const uint32_t*>(original_esp + 4);   // argument 0
    }
    if (ecx < 0x10000 || run < 0x10000) return;

    std::string raw;
    std::string text;
    uintptr_t end = 0;
    const unsigned skip = RunTextOffset(run, g_config->message_text_skip);
    if (!ReadRun(run + skip, &raw, &text, &end)) {
        // The hook ran on a message but nothing readable was there: say so instead of letting
        // the line disappear from the log without a trace.
        if (g_config->diagnostics) {
            char msg[384];
            _snprintf(msg, sizeof(msg),
                      "message 0x%08x: nothing readable at 0x%08x (+%u) - bytes: %s",
                      static_cast<unsigned>(ecx), static_cast<unsigned>(run + skip), skip,
                      HexAt(run + skip, 24).c_str());
            g_sink->Diagnostic(msg);
        }
        return;
    }

    // The same run is set again while the box is measured and redrawn: only skip it when both
    // the pointer *and* the content match a recent call (see the comment on RecentRun above).
    const DWORD now = GetTickCount();
    const uint32_t hash = HashBytes(raw);
    for (int i = 0; i < kRecentRuns; ++i) {
        if (g_recent[i].run == static_cast<uint32_t>(run) && g_recent[i].hash == hash
            && (now - g_recent[i].ms) < kRecentWindowMs) {
            if (g_config->diagnostics) {
                char msg[160];
                _snprintf(msg, sizeof(msg),
                          "message 0x%08x: run 0x%08x redrawn within %u ms, not re-logged",
                          static_cast<unsigned>(ecx), static_cast<unsigned>(run),
                          static_cast<unsigned>(now - g_recent[i].ms));
                g_sink->Diagnostic(msg);
            }
            return;
        }
    }
    // A page that was already logged as part of an earlier message (see g_ahead).
    for (int i = 0; i < kAheadPages; ++i) {
        if (g_ahead[i].run == static_cast<uint32_t>(run + skip) && g_ahead[i].hash == hash
            && (now - g_ahead[i].ms) < kAheadWindowMs) {
            if (g_config->diagnostics) {
                char msg[160];
                _snprintf(msg, sizeof(msg),
                          "message 0x%08x: run 0x%08x is a page already logged with its message",
                          static_cast<unsigned>(ecx), static_cast<unsigned>(run));
                g_sink->Diagnostic(msg);
            }
            return;
        }
    }
    g_recent[g_recent_pos].run = static_cast<uint32_t>(run);
    g_recent[g_recent_pos].hash = hash;
    g_recent[g_recent_pos].ms = now;
    g_recent_pos = (g_recent_pos + 1) % kRecentRuns;

    // A new message on this box: the pages of its previous one that are still held were shown (or
    // skipped through) - they go out first, so the log keeps the game's order.
    ReleaseHeld(ecx, false);

    const std::string plate =
        g_config->diagnostics ? ReadAt(ecx + g_config->message_plate_offset, 48) : std::string();
    CatalogEntry e;
    e.raw = raw;
    e.text = text;
    g_sink->LineFromObject(e, ecx, g_config->message_plate_offset, run, "message");
    if (g_config->diagnostics) {
        char msg[384];
        _snprintf(msg, sizeof(msg), "message state=0x%08x run=0x%08x at=+%u plate=\"%s\" text=\"%.60s\"",
                  static_cast<unsigned>(ecx), static_cast<unsigned>(run), skip, plate.c_str(),
                  text.substr(0, 60).c_str());
        g_sink->Diagnostic(msg);
    }

    // The message's other pages: one line each, in order, with the same speaker - held until the
    // game shows them (see HeldPage).
    for (int page = 2; page <= 32; ++page) {
        const uintptr_t brk = end;
        const uintptr_t next = NextPage(end);
        if (next == 0) break;
        std::string page_raw;
        std::string page_text;
        if (!ReadRun(next, &page_raw, &page_text, &end)) {
            // a page of codes only (e.g. "#E0#M0" before a pause): nothing to log, keep walking
            if (!page_raw.empty() && end != 0) continue;
            break;
        }
        g_ahead[g_ahead_pos].run = static_cast<uint32_t>(next);
        g_ahead[g_ahead_pos].hash = HashBytes(page_raw);
        g_ahead[g_ahead_pos].ms = now;
        g_ahead_pos = (g_ahead_pos + 1) % kAheadPages;

        HeldPage held;
        held.state = ecx;
        held.base = run;
        held.brk = static_cast<uint32_t>(brk - run);
        held.text_at = static_cast<uint32_t>(next - run);
        held.page = page;
        held.held_ms = now;
        held.entry.raw = page_raw;
        held.entry.text = page_text;
        held.address = next;
        if (InterlockedCompareExchange(&g_held_ready, 0, 0) != 0) {
            EnterCriticalSection(&g_held_lock);
            g_held.push_back(held);
            LeaveCriticalSection(&g_held_lock);
        } else {
            g_sink->LineFromObject(held.entry, ecx, g_config->message_plate_offset, next, "message");
        }
        if (g_config->diagnostics) {
            char msg[384];
            _snprintf(msg, sizeof(msg),
                      "message state=0x%08x run=0x%08x page %d at 0x%08x held text=\"%.60s\"",
                      static_cast<unsigned>(ecx), static_cast<unsigned>(run), page,
                      static_cast<unsigned>(next), page_text.substr(0, 60).c_str());
            g_sink->Diagnostic(msg);
        }
    }
}

}  // namespace

// ---------------------------------------------------------------- C linkage pieces
// The detour keeps the register snapshot layout the tools expect:
//   snap[0] unused (the pushed index), snap[1..8] = edi, esi, ebp, esp, ebx, edx, ecx, eax,
//   snap[9] = eflags. [esp] on entry is the return address, [esp+4] the first argument.
extern "C" {
void* g_atmt_trampoline = nullptr;
void atmt_probe_message(void* snapshot);

__attribute__((naked, used)) void atmt_detour_message() {
    __asm__ volatile(
        "pushfl\n\t"
        "pushal\n\t"
        "pushl $0\n\t"
        "pushl %esp\n\t"
        "call _atmt_probe_message\n\t"
        "addl $8, %esp\n\t"
        "popal\n\t"
        "popfl\n\t"
        "jmp *_g_atmt_trampoline\n\t");
}

void atmt_probe_message(void* snapshot) {
    atmt::Probe(snapshot);
}
}  // extern "C"

bool InstallAddressHooks(const Config& config, LogSink& sink) {
#if !defined(__i386__)
    sink.Note("address hooks are only implemented for the 32-bit build");
    return false;
#else
    if (Hooks().create == nullptr) {
        sink.Note("no hook service (this mod needs the loader)");
        return false;
    }
    if (config.message_hook_address == 0) {
        sink.Note("MessageHook is not set - nothing to hook");
        return false;
    }
    g_config_storage = config;   // our own copy: the caller's may be a local
    g_sink = &sink;
    if (InterlockedCompareExchange(&g_held_ready, 0, 0) == 0) {
        InitializeCriticalSection(&g_held_lock);
        InterlockedExchange(&g_held_ready, 1);
    }
    void* target = reinterpret_cast<void*>(config.message_hook_address);
    if (!IsExecutableCode(target)) {
        char msg[160];
        _snprintf(msg, sizeof(msg), "skipping 0x%08x : not executable code",
                  static_cast<unsigned>(config.message_hook_address));
        sink.Note(msg);
        HostLogError((std::string("dialog logger: ") + msg).c_str());
        return false;
    }
    // The default address is checked against the supported exe's bytes: push ebp; mov ebp,esp;
    // fldz; mov eax,[ebp+8] (SenPatcher, v1.3.1 and its current sources, does not patch it). An
    // address set by hand in the ini is the user's own call.
    if (config.message_hook_address == Config().message_hook_address) {
        static const atmt_code::Expect kExpected[] = {
            {0x00701E50, "\x55\x8b\xec\xd9\xee\x8b\x45\x08", 8, "the game's message setter"},
        };
        char why[256];
        if (!atmt_code::CheckAll(kExpected, why, sizeof(why))) {
            sink.Note(why);
            HostLogError((std::string("dialog logger: ") + why + " - not hooked").c_str());
            return false;
        }
    }
    void* trampoline = nullptr;
    void* handle = Hooks().create(target, reinterpret_cast<void*>(&atmt_detour_message),
                                  &trampoline);
    // The service returns the target address as its handle, so only null means failure.
    // (Reading any non-zero value as failure left the hook created but never enabled: the
    // mod ran, said "could not hook", and logged nothing - the self test pins this now.)
    if (handle == nullptr) {
        char msg[160];
        _snprintf(msg, sizeof(msg), "could not hook 0x%08x",
                  static_cast<unsigned>(config.message_hook_address));
        sink.Note(msg);
        HostLogError((std::string("dialog logger: ") + msg).c_str());
        return false;
    }
    g_atmt_trampoline = trampoline;
    g_hook_target = handle;
    if (Hooks().enable != nullptr && Hooks().enable(handle) != 0) {
        char msg[160];
        _snprintf(msg, sizeof(msg), "hook created at 0x%08x but not enabled",
                  static_cast<unsigned>(config.message_hook_address));
        sink.Note(msg);
        HostLogError((std::string("dialog logger: ") + msg).c_str());
        return false;
    }
    char msg[160];
    _snprintf(msg, sizeof(msg), "hook installed at 0x%08x (the game's message setter)",
              static_cast<unsigned>(config.message_hook_address));
    sink.Note(msg);
    return true;
#endif
}

const Config& HookConfig() {
    return g_config_storage;
}

void ReleaseHeldPages(bool all) {
    ReleaseHeld(0, all);
}

// Takes this build's hook on the game's message setter away again. Only the dev reload calls this
// (docs/OVERLAY.md): MinHook refuses a second hook on the same address, so the build that takes
// over has to wait until this one is gone. Disable-then-remove is the order MinHook makes safe
// against a game thread that is inside the hook right now.
bool UninstallAddressHooks() {
    if (g_hook_target == nullptr) return false;
    if (Hooks().disable != nullptr) Hooks().disable(g_hook_target);
    if (Hooks().remove != nullptr) Hooks().remove(g_hook_target);
    g_hook_target = nullptr;
    g_atmt_trampoline = nullptr;
    if (g_sink != nullptr) g_sink->Note("hook removed (a new build takes over)");
    return true;
}

}  // namespace atmt

