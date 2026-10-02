// logsink.cpp - writes atmt_dialogs.jsonl and atmt_latest.txt next to the game.
#include "atmt.h"

#include <cstdio>
#include <ctime>
#include <string>

namespace atmt {

namespace {
std::wstring g_game_dir_override;
std::wstring g_mod_dir_override;

// A NUL-terminated, printable string from the game's memory - the name plate of the message
// state. Bounded and defensive, because the address comes from the game.
std::string ReadCStringAt(uintptr_t address, unsigned max_len) {
    std::string out;
    if (address < 0x10000) return out;
    for (unsigned i = 0; i < max_len; ++i) {
        const uintptr_t p = address + i;
        if (i % 32 == 0) {
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery(reinterpret_cast<const void*>(p), &mbi, sizeof(mbi)) == 0) break;
            if (mbi.State != MEM_COMMIT) break;
            if ((mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) break;
        }
        const char c = *reinterpret_cast<const char*>(p);
        if (c == '\0') break;
        if (static_cast<unsigned char>(c) < 0x20) break;
        out.push_back(c);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}
}  // namespace

void SetHostPaths(const wchar_t* game_dir, const wchar_t* mod_dir) {
    if (game_dir != nullptr) g_game_dir_override = game_dir;
    if (mod_dir != nullptr) g_mod_dir_override = mod_dir;
}

static std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    _snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

std::wstring GetGameDir() {
    static std::wstring cached;
    if (!g_game_dir_override.empty()) return g_game_dir_override;
    if (!cached.empty()) return cached;
    wchar_t buf[MAX_PATH];
    const DWORD n = GetModuleFileNameW(GetModuleHandleW(nullptr), buf, MAX_PATH);
    if (n > 0) {
        std::wstring path(buf, n);
        const size_t slash = path.find_last_of(L"\\/");
        if (slash != std::wstring::npos) path.resize(slash);
        cached = path;
    }
    return cached;
}

std::wstring GetSelfDir() {
    static std::wstring cached;
    if (!g_mod_dir_override.empty()) return g_mod_dir_override;
    if (!cached.empty()) return cached;
    HMODULE self = static_cast<HMODULE>(SelfModuleHandle());
    if (self == nullptr) {
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&GetSelfDir), &self);
    }
    if (self != nullptr) {
        wchar_t buf[MAX_PATH];
        const DWORD n = GetModuleFileNameW(self, buf, MAX_PATH);
        if (n > 0) {
            std::wstring path(buf, n);
            const size_t slash = path.find_last_of(L"\\/");
            if (slash != std::wstring::npos) path.resize(slash);
            cached = path;
        }
    }
    if (cached.empty()) cached = GetGameDir();
    return cached;
}

bool LogSink::Open(const std::wstring& dir) {
    InitializeCriticalSection(&lock_);
    InitializeCriticalSection(&pending_lock_);
    ready_ = true;
    if (!write_files_) {
        // LogToFile=false: nothing is created, nothing is truncated, nothing is written. The lines
        // are still recorded for the panel (history_) and counted, so the mod behaves identically
        // from the player's side - it just leaves no trace on disk.
        return true;
    }
    log_dir_ = dir;
    const std::wstring jsonl = dir + L"\\atmt_dialogs.jsonl";
    const std::wstring latest = dir + L"\\atmt_latest.txt";
    const bool fresh = (_wfopen(jsonl.c_str(), L"rb") == nullptr);
    latest_path_ = latest;
    if (FILE* trunc = _wfopen(latest.c_str(), L"wb")) fclose(trunc);
    jsonl_ = _wfopen(jsonl.c_str(), L"ab");
    if (jsonl_ == nullptr) {
        ready_ = false;
        return false;
    }
    if (fresh) {
        fputs("{\"seq\":0,\"event\":\"session_start\",\"game\":\"Trails of Cold Steel\"}\n",
              jsonl_);
        fflush(jsonl_);
    }
    return true;
}

void LogSink::Line(const CatalogEntry& e, uintptr_t address, const std::string& speaker,
                   const char* source) {
    if (!ready_) return;
    // Straight write from the game's own thread. There is no lookup here by design: the text
    // and the speaker both came from the game (see addr_hook.cpp), so writing is all that is
    // left to do.
    EnterCriticalSection(&lock_);
    ++count_;
    time_t now = time(nullptr);
    tm tmv;
    localtime_s(&tmv, &now);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", &tmv);

    const std::string& text = e.text.empty() ? e.raw : e.text;
    if (jsonl_ != nullptr) {
        // script/offset/image are kept as fixed values rather than dropped from the schema: some of
        // tools/*.py still key off their presence, even though nothing populates them any more (the
        // mod dropped the catalog and the screenshot feature both read from).
        fprintf(jsonl_,
                "{\"seq\":%llu,\"time\":\"%s\",\"speaker\":\"%s\",\"text\":\"%s\",\"raw\":\"%s\","
                "\"script\":\"\",\"offset\":0,\"address\":\"0x%llx\",\"image\":\"\","
                "\"source\":\"%s\"}\n",
                static_cast<unsigned long long>(count_), ts, JsonEscape(speaker).c_str(),
                JsonEscape(text).c_str(), JsonEscape(e.raw).c_str(),
                static_cast<unsigned long long>(address),
                source != nullptr ? source : "message");
        fflush(jsonl_);
    }

    // One place decides what a "line" is: the jsonl record above, the small atmt_latest.txt tail
    // below and the overlay panel all come from this. The panel reads it from memory, which is why
    // it shows exactly what was logged (and nothing else).
    LogLine line;
    line.seq = count_;
    {
        char clock[16];
        strftime(clock, sizeof(clock), "%H:%M:%S", &tmv);
        line.time = clock;
    }
    line.speaker = speaker;
    line.text = text;
    if (history_ != nullptr) history_->Push(line);

    if (!latest_path_.empty()) {
        // kept small: it is meant for a live tail / OBS overlay
        latest_lines_.push_back(line.Display());
        while (latest_lines_.size() > kMaxLatestLines) latest_lines_.pop_front();
        if (FILE* f = _wfopen(latest_path_.c_str(), L"wb")) {
            for (const std::string& l : latest_lines_) {
                fwrite(l.data(), 1, l.size(), f);
                fputc('\n', f);
            }
            fclose(f);
        }
    }
    LeaveCriticalSection(&lock_);
}

void LogSink::LineFromObject(const CatalogEntry& e, uintptr_t obj, unsigned plate_offset,
                             uintptr_t address, const char* source) {
    if (!ready_) return;
    // The speaker is the name plate the game itself is about to draw. It is read from the
    // message state; if the game has not written it yet (it does that a moment later), the
    // line waits and the flush reads the same field again - still the game's data.
    const std::string plate =
        obj > 0x10000 ? ReadCStringAt(obj + plate_offset, 48) : std::string();
    if (pending_ms_ != 0 && plate.empty()) {
        PendingLine evicted;
        bool have_evicted = false;
        EnterCriticalSection(&pending_lock_);
        if (pending_count_ >= kMaxPending) {
            evicted = pending_[0];
            for (size_t i = 1; i < pending_count_; ++i) pending_[i - 1] = pending_[i];
            --pending_count_;
            have_evicted = true;
        }
        PendingLine& p = pending_[pending_count_++];
        p.entry = e;
        p.address = address;
        p.speaker.clear();
        p.source = source != nullptr ? source : "message";
        p.deadline = GetTickCount() + pending_ms_;
        p.obj = obj;
        p.plate_offset = plate_offset;
        LeaveCriticalSection(&pending_lock_);
        if (have_evicted) {
            Line(evicted.entry, evicted.address, evicted.speaker, evicted.source.c_str());
        }
        return;
    }

    Line(e, address, plate, source);
}

void LogSink::FlushPending() {
    if (!ready_) return;
    const DWORD now = GetTickCount();
    PendingLine due[kMaxPending];
    size_t due_count = 0;
    EnterCriticalSection(&pending_lock_);
    size_t keep = 0;
    for (size_t i = 0; i < pending_count_; ++i) {
        PendingLine& p = pending_[i];
        if (now >= p.deadline) {
            due[due_count++] = p;
        } else {
            if (keep != i) pending_[keep] = p;
            ++keep;
        }
    }
    pending_count_ = keep;
    LeaveCriticalSection(&pending_lock_);
    for (size_t i = 0; i < due_count; ++i) {
        // In order, and with the plate read once more: that is when the game has set it.
        const std::string plate = due[i].obj > 0x10000
                                      ? ReadCStringAt(due[i].obj + due[i].plate_offset, 48)
                                      : std::string();
        Line(due[i].entry, due[i].address, plate, due[i].source.c_str());
    }
}

void LogSink::Note(const std::string& message) {
    if (!ready_ || jsonl_ == nullptr) return;   // a note has nowhere to go without the file
    EnterCriticalSection(&lock_);
    fprintf(jsonl_, "{\"event\":\"note\",\"message\":\"%s\"}\n", JsonEscape(message).c_str());
    fflush(jsonl_);
    LeaveCriticalSection(&lock_);
}

void LogSink::Diagnostic(const std::string& message) {
    // log_dir_ is only set when the sink may write files, so LogToFile=false silences this too.
    if (log_dir_.empty()) return;
    EnterCriticalSection(&lock_);
    const std::wstring path = log_dir_ + L"\\atmt_diagnostics.log";
    if (FILE* f = _wfopen(path.c_str(), L"ab")) {
        time_t now = time(nullptr);
        tm tmv;
        localtime_s(&tmv, &now);
        char ts[32];
        strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
        fprintf(f, "[%s] %s\n", ts, message.c_str());
        fclose(f);
    }
    LeaveCriticalSection(&lock_);
}

void LogSink::Close() {
    if (!ready_) return;
    // Lines that are still waiting go out now, so stopping the game never drops the last one.
    EnterCriticalSection(&pending_lock_);
    for (size_t i = 0; i < pending_count_; ++i) pending_[i].deadline = 0;
    LeaveCriticalSection(&pending_lock_);
    FlushPending();
    if (jsonl_ != nullptr) {
        fclose(jsonl_);
        jsonl_ = nullptr;
    }
    ready_ = false;
}

}  // namespace atmt

