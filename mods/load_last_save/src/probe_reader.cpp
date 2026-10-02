// probe_reader.cpp - who asks the game to read a save file?
//
// Off unless `ProbeReader=true` in the mod's ini. This is the last piece of evidence for the
// "-al" mod: the file probe showed that a slot's save is read by the function at 0x00485000
// (a 460,992-byte read of `<dir>/saveNNN.dat`), but *who calls it* is hidden behind the game's
// dispatch tables, so static tracing dead-ends.
//
// This hooks exactly that one function and records, at the moment of a real load:
//   * the arguments the caller passed,
//   * the file name the reader is about to open (the global it formats the path from),
//   * the save manager object and its directory pointer,
//   * the return addresses on the stack that land inside ed8.exe - the caller chain.
//
// The detour is naked and only observes: it saves the flags and registers, calls a logging
// function, restores everything and jumps to the trampoline. Nothing about the game's behaviour
// changes, and no calling convention is disturbed (the original still does its own cleanup).
#include "al.h"
#include "mod_api.h"

#include <cstdarg>
#include <cstdio>
#include <cwchar>

namespace al {

namespace {

const AtmtModApi* g_api = nullptr;
const AtmtModApi* main_api = nullptr;
void* g_reader_trampoline = nullptr;

// From the file probe: this function reads a slot's save file (the whole 460,992 bytes).
const uintptr_t kReaderVa = 0x00485000;
// The save manager object (its pointer lives here) and the buffer it formats the file name into.
const uintptr_t kManagerPtrVa = 0x00C3E750;
const uintptr_t kNameBufVa = 0x00C3E758;

void LogLine(const char* fmt, ...) {
    if (g_api == nullptr || g_api->log == nullptr) return;
    char buf[900];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(buf, sizeof(buf) - 1, fmt, args);
    va_end(args);
    buf[sizeof(buf) - 1] = '\0';
    g_api->log(buf);
}

bool Readable(const void* p, size_t n) {
    if (p == nullptr) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(p, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT) return false;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    const uint8_t* start = static_cast<const uint8_t*>(p);
    return start + n <= static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize;
}

std::string ReadCStringAt(uintptr_t p, unsigned max_len) {
    std::string out;
    for (unsigned i = 0; i < max_len && Readable(reinterpret_cast<const void*>(p + i), 1); ++i) {
        const char c = *reinterpret_cast<const char*>(p + i);
        if (c == '\0') break;
        out.push_back(c >= 0x20 && c < 0x7f ? c : '.');
    }
    return out;
}

// The caller chain of a hooked call, from a register snapshot: the EBP chain first (every frame is
// inside the stack, and this sees through the game's jump thunks, which push no frames), then a
// stack scan as a fallback - with *every* read guarded, because a worker thread's stack can end
// just above esp (an unguarded 1 KB scan there crashed the game once, at offset 0x6c04).
bool InGameModule(uintptr_t value) {
    if (value < 0x10000) return false;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(reinterpret_cast<const void*>(value), &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT || mbi.Type != MEM_IMAGE) return false;
    if (reinterpret_cast<uintptr_t>(mbi.AllocationBase) != 0x00400000) return false;   // ed8.exe
    // ...and it must be executable code: ed8.exe's image is 16 MB, but its code ends near 0xB37978,
    // and the first version of this accepted *data* addresses inside the image (a stack value that
    // happened to look like a frame).
    const DWORD executable = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE
                             | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & executable) != 0;
}

// The raw stack words at the hook's entry (guarded, and only the first few): the frame chain is
// often unusable in this game (much of it is built without frame pointers), and seeing the words
// themselves is what lets the caller be identified from the module list.
std::string RawStack(const unsigned char* snapshot, unsigned words) {
    std::string out;
    const uintptr_t* snap = reinterpret_cast<const uintptr_t*>(snapshot);
    const uintptr_t entry_esp = snap[1 + 3] + 4;
    for (unsigned i = 0; i < words; ++i) {
        const uintptr_t addr = entry_esp + i * 4;
        if (!Readable(reinterpret_cast<const void*>(addr), 4)) break;
        const uintptr_t value = *reinterpret_cast<const uintptr_t*>(addr);
        char word[16];
        _snprintf(word, sizeof(word), " %08x", static_cast<unsigned>(value));
        out += word;
    }
    return out;
}

std::string StackFrames(unsigned char* snapshot, unsigned max_frames) {
    std::string out;
    if (snapshot == nullptr) return out;
    const uintptr_t* snap = reinterpret_cast<const uintptr_t*>(snapshot);
    const uintptr_t saved_ebp = snap[1 + 2];
    const uintptr_t entry_esp = snap[1 + 3] + 4;   // the saved ESP is entry-4 (after pushfl)

    unsigned found = 0;
    uintptr_t ebp = saved_ebp;
    for (unsigned i = 0; i < 12 && found < max_frames; ++i) {
        if (!Readable(reinterpret_cast<const void*>(ebp), 8)) break;
        const uintptr_t ret = *reinterpret_cast<const uintptr_t*>(ebp + 4);
        if (InGameModule(ret)) {
            char frame[16];
            _snprintf(frame, sizeof(frame), " 0x%08x", static_cast<unsigned>(ret));
            out += frame;
            ++found;
        }
        const uintptr_t next = *reinterpret_cast<const uintptr_t*>(ebp);
        if (next <= ebp || next - ebp > 0x10000) break;   // not a sane frame link
        ebp = next;
    }

    if (found < max_frames) {
        for (unsigned i = 4; i < 128 && found < max_frames; ++i) {
            const uintptr_t addr = entry_esp + i * 4;
            if (!Readable(reinterpret_cast<const void*>(addr), 4)) break;
            const uintptr_t value = *reinterpret_cast<const uintptr_t*>(addr);
            if (InGameModule(value)) {
                char frame[16];
                _snprintf(frame, sizeof(frame), " 0x%08x", static_cast<unsigned>(value));
                out += frame;
                ++found;
            }
        }
    }
    return out;
}

}  // namespace

std::string HookStackFrames(const void* snapshot, unsigned max_frames) {
    return StackFrames(const_cast<unsigned char*>(static_cast<const unsigned char*>(snapshot)),
                       max_frames);
}

// Called from the naked detour with a snapshot of the registers.
void ProbeReader(void* snapshot) {
    uintptr_t* snap = reinterpret_cast<uintptr_t*>(snapshot);
    const uintptr_t entry_esp = snap[1 + 3] + 4;
    if (entry_esp < 0x10000 || !Readable(reinterpret_cast<const void*>(entry_esp), 20)) return;
    const uintptr_t caller = *reinterpret_cast<const uintptr_t*>(entry_esp);
    const uintptr_t a0 = *reinterpret_cast<const uintptr_t*>(entry_esp + 4);
    const uintptr_t a1 = *reinterpret_cast<const uintptr_t*>(entry_esp + 8);
    const uintptr_t a2 = *reinterpret_cast<const uintptr_t*>(entry_esp + 12);
    const uintptr_t a3 = *reinterpret_cast<const uintptr_t*>(entry_esp + 16);

    const std::string name = ReadCStringAt(kNameBufVa, 128);
    SetLastGameReadName(name);
    NotifySaveSystemReady();
    uintptr_t manager = 0;
    if (Readable(reinterpret_cast<const void*>(kManagerPtrVa), 4)) {
        manager = *reinterpret_cast<const uintptr_t*>(kManagerPtrVa);
    }
    std::string dir;
    if (manager != 0 && Readable(reinterpret_cast<const void*>(manager + 0x5c), 4)) {
        const uintptr_t tagged = *reinterpret_cast<const uintptr_t*>(manager + 0x5c);
        dir = ReadCStringAt(tagged & ~static_cast<uintptr_t>(1), 128);
    }

    const std::string frames = StackFrames(reinterpret_cast<unsigned char*>(snapshot), 6);
    const std::string raw = RawStack(static_cast<const unsigned char*>(snapshot), 20);
    char line[900];
    _snprintf(line, sizeof(line),
              "reader: args=0x%08x 0x%08x 0x%08x 0x%08x caller=0x%08x name=\"%s\" manager=0x%08x "
              "dir=\"%s\" game:%s stack:%s",
              static_cast<unsigned>(a0), static_cast<unsigned>(a1), static_cast<unsigned>(a2),
              static_cast<unsigned>(a3), static_cast<unsigned>(caller), name.c_str(),
              static_cast<unsigned>(manager), dir.c_str(), frames.c_str(), raw.c_str());
    LogLine(line);
}

}  // namespace al

extern "C" {
void probe_reader_entry(void* snapshot);
void* g_reader_trampoline_ptr = nullptr;

__attribute__((naked, used)) void atmt_detour_reader() {
    __asm__ volatile(
        "pushfl\n\t"
        "pushal\n\t"
        "pushl $0\n\t"
        "pushl %esp\n\t"
        "call _probe_reader_entry\n\t"
        "addl $8, %esp\n\t"
        "popal\n\t"
        "popfl\n\t"
        "jmp *_g_reader_trampoline_ptr\n\t");
}

void probe_reader_entry(void* snapshot) {
    al::ProbeReader(snapshot);
}
}  // extern "C"

namespace al {

bool InstallReaderProbe(const AtmtModApi* api, std::string* why_not) {
    if (api == nullptr || api->hook_create == nullptr || api->hook_enable == nullptr) {
        if (why_not != nullptr) *why_not = "no hook service from the loader";
        return false;
    }
    g_api = api;
    void* target = reinterpret_cast<void*>(kReaderVa);
    void* trampoline = nullptr;
    void* handle = api->hook_create(target, reinterpret_cast<void*>(&atmt_detour_reader), &trampoline);
    if (handle == nullptr) {
        if (why_not != nullptr) *why_not = "could not hook the save reader (0x00485000)";
        return false;
    }
    g_reader_trampoline_ptr = trampoline;
    main_api = api;
    api->hook_enable(handle);
    // The mod calls the game through this trampoline when it performs the load, and it learns from
    // this hook when the game's save subsystem is up (the game reading a file itself).
    SetSaveReaderTrampoline(trampoline);
    SetLoadLog(reinterpret_cast<void*>(api->log));
    LogLine("reader probe: watching 0x00485000 (the function that reads a save file)");
    return true;
}

}  // namespace al

