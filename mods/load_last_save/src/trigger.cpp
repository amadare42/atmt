// trigger.cpp - asking the game to load a save, when the title route (title_load.cpp) is not the
// one doing it.
//
// TriggerLoad() here is the fallback: it only runs if the title hook could not be installed (see
// mod_entry.cpp). It writes the chosen save's name into the game's own name buffer and raises the
// request flag the game's save-file reader waits on, so the game's own worker thread reads the file -
// but nothing installs the menu's completion callback, so the game does not *enter* the session by
// itself; it only gets the file read. Every fact it uses was measured in the running game
// (docs/ENGINE_NOTES.md, "Saving and loading"):
//
//   * a slot is read by the function at 0x00485000 ("the reader"). It takes no arguments: it uses
//     the save manager object at 0x00C3E750 (which holds the save directory) and the file *name*
//     in a global buffer at 0x00C3E758, formats `<dir>/<name>`, reads and parses it. Observed
//     live: name="save063.dat" the moment a slot was loaded, name="save511.dat" when the game read
//     its own system file at startup.
//   * so "load slot N" = put the slot's file name in that buffer and call the reader. The reader
//     waits for the subsystem to be idle, which is why calling it from our own thread is
//     acceptable - the game itself calls it from a loader thread.
//   * the address is stable (ed8.exe has no ASLR), and this only happens when `-al` was given.
//
// The call goes through the trampoline the loader's hook created, which runs the original prologue
// and jumps into the body: that is the function itself, not a copy.
#include "al.h"

#include <cstdio>

namespace al {

namespace {

void* g_reader_trampoline = nullptr;   // set by the reader probe (probe_reader.cpp)
void* g_log = nullptr;                 // the loader's log function
volatile LONG g_save_system_ready = 0;
volatile LONG g_load_issued = 0;
volatile LONG g_name_seq = 0;
char g_last_name[64] = {0};
// How long to wait after the save subsystem is up before requesting the load.
unsigned g_start_delay_ms = kLoadDelayMs;

std::string LeafName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const std::wstring leaf = slash == std::wstring::npos ? path : path.substr(slash + 1);
    std::string out;
    for (wchar_t c : leaf) out.push_back(c < 128 ? static_cast<char>(c) : '?');
    return out;
}

void LogText(const std::string& text) {
    if (g_log != nullptr) {
        reinterpret_cast<void(__cdecl*)(const char*)>(g_log)(text.c_str());
    }
}

// Writes the file name into the game's buffer; returns what was there before, so the log shows
// both (if the game's own idea of the slot differed from ours, that is visible immediately).
std::string WriteNameBuffer(const std::string& name) {
    std::string previous;
    char* buffer = reinterpret_cast<char*>(kGameSaveNameBuffer);
    for (int i = 0; i < 64; ++i) {
        const char c = buffer[i];
        if (c == '\0') break;
        previous.push_back(c >= 0x20 && c < 0x7f ? c : '?');
    }
    _snprintf(buffer, 63, "%s", name.c_str());
    buffer[63] = '\0';
    return previous;
}

}  // namespace

void SetSaveReaderTrampoline(void* trampoline) {
    g_reader_trampoline = trampoline;
}

void SetLoadLog(void* log_function) {
    g_log = log_function;
}

void NotifySaveSystemReady() {
    InterlockedExchange(&g_save_system_ready, 1);
}

bool SaveSystemReady() {
    return g_save_system_ready != 0;
}

void SetLastGameReadName(const std::string& name) {
    // Written from the game's thread while the mod reads it: keep it to one small buffer with a
    // sequence counter, so a torn read is impossible to act on.
    InterlockedIncrement(&g_name_seq);
    _snprintf(g_last_name, sizeof(g_last_name) - 1, "%s", name.c_str());
    g_last_name[sizeof(g_last_name) - 1] = '\0';
    InterlockedIncrement(&g_name_seq);
}

std::string LastGameReadName() {
    std::string out;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const LONG before = g_name_seq;
        if (before % 2 != 0) continue;          // a write is in progress
        out.assign(g_last_name);
        if (g_name_seq == before) return out;   // unchanged while reading
    }
    return out;
}

// "save063.dat" -> 63. The name is "save%03d.dat" (11 characters - a >= 12 guard here silently
// rejected every real save once, so the length is checked against the shortest valid form). Autosaves
// are named differently ("autosaveNN.dat") and are not reached by the game's format string, so they
// take the fallback route.
unsigned SaveSlotNumber(const std::string& file_name) {
    if (file_name.size() < 9) return kNoSlot;      // "save0.dat" is the shortest plausible form
    if (_strnicmp(file_name.c_str(), "save", 4) != 0) return kNoSlot;
    if (_stricmp(file_name.c_str() + file_name.size() - 4, ".dat") != 0) return kNoSlot;
    const size_t digits = file_name.size() - 4 - 4;   // "save" + digits + ".dat"
    if (digits == 0 || digits > 4) return kNoSlot;
    unsigned value = 0;
    for (size_t i = 4; i < 4 + digits; ++i) {
        const char c = file_name[i];
        if (c < '0' || c > '9') return kNoSlot;
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    return value;
}

SaveTarget ParseSaveTarget(const std::string& file_name) {
    SaveTarget target;
    const unsigned slot = SaveSlotNumber(file_name);
    if (slot != kNoSlot) {
        // 511 is the system file (save511.dat), which the load menu never lists.
        if (slot != 511) target.slot = slot;
        return target;
    }
    // "autosaveNN.dat": the game formats the number with %02d, so two or three digits come back.
    if (file_name.size() < 14 || _strnicmp(file_name.c_str(), "autosave", 8) != 0) return target;
    if (_stricmp(file_name.c_str() + file_name.size() - 4, ".dat") != 0) return target;
    const size_t digits = file_name.size() - 8 - 4;
    if (digits < 2 || digits > 3) return target;
    unsigned value = 0;
    for (size_t i = 8; i < 8 + digits; ++i) {
        const char c = file_name[i];
        if (c < '0' || c > '9') return target;
        value = value * 10 + static_cast<unsigned>(c - '0');
    }
    target.slot = value;
    target.autosave = true;
    return target;
}

void SetStartDelayMs(unsigned ms) {
    g_start_delay_ms = ms;
}

unsigned StartDelayMs() {
    return g_start_delay_ms;
}

bool TriggerLoad(const std::wstring& path, std::string* why_not) {
    const std::string leaf = LeafName(path);
    if (leaf.empty()) {
        if (why_not != nullptr) *why_not = "no file name to load";
        return false;
    }

    // The load has to happen when the game's *menu* would do it, not during boot: writing the game's
    // save state while its own boot load is still in flight has left the game spinning before it
    // presents a single frame (2026-09-22, measured against the state-machine route this fallback used
    // to share code with). StartDelayMs in the ini is that wait.
    if (g_start_delay_ms != 0) {
        char line[160];
        _snprintf(line, sizeof(line), "al: waiting %u ms before the load (StartDelayMs)",
                  g_start_delay_ms);
        LogText(line);
        Sleep(g_start_delay_ms);
    }

    if (g_reader_trampoline == nullptr) {
        if (why_not != nullptr) *why_not = "the game's save reader was not hooked (ProbeReader?)";
        return false;
    }
    if (InterlockedCompareExchange(&g_load_issued, 1, 0) != 0) {
        if (why_not != nullptr) *why_not = "a load was already issued";
        return false;
    }

    // Wait until the game's save subsystem exists at all (it has read a file by itself) - the
    // manager and the save folder are then resolved, so writing a name and raising the request flag
    // means something.
    for (unsigned waited = 0; waited < 30000 && g_save_system_ready == 0; waited += 100) {
        Sleep(100);
    }
    if (g_save_system_ready == 0) {
        if (why_not != nullptr) *why_not = "the game never read a save file by itself";
        return false;
    }

    // The request: the file name in the game's buffer, and the flag the reader waits on. The game's
    // own machinery does the rest - this mod never calls into the game (that blocked once).
    const std::string previous = WriteNameBuffer(leaf);
    volatile unsigned char* flag =
        reinterpret_cast<volatile unsigned char*>(kGameSaveReadyFlag);
    *flag = 1;   // the load request: the game's own machinery picks this up
    {
        char line[320];
        _snprintf(line, sizeof(line),
                  "al: requested a load of %s (name buffer held \"%s\", request flag set)",
                  leaf.c_str(), previous.c_str());
        LogText(line);
    }

    // Start the game's save worker as its own thread, exactly as the game does: the request state
    // (name + flag) is set above, and the worker picks it up, reads the file and parses it.
    HANDLE worker = CreateThread(nullptr, 0,
                                 reinterpret_cast<LPTHREAD_START_ROUTINE>(kGameSaveReader), nullptr,
                                 0, nullptr);
    if (worker == nullptr) {
        if (why_not != nullptr) *why_not = "could not start the game's save worker thread";
        return false;
    }
    CloseHandle(worker);
    LogText("al: started the game's save worker thread for " + leaf);

    // Watch for the game actually reading it: that is the proof the request was taken.
    for (unsigned waited = 0; waited < 25000; waited += 100) {
        if (LastGameReadName() == leaf) {
            char line[320];
            _snprintf(line, sizeof(line), "al: the game started reading %s (request picked up)",
                      leaf.c_str());
            LogText(line);
            return true;
        }
        Sleep(100);
    }
    if (why_not != nullptr) *why_not = "the worker was started but the file was not read";
    return false;
}

}  // namespace al

