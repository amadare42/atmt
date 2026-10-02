// util.cpp - see util.h.
#include "util.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <random>
#include <sstream>

#ifdef _WIN32
#include <io.h>       // _commit
#else
#include <unistd.h>   // fsync
#endif

namespace atmt {

fs::path Path(const std::string& utf8) {
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(utf8.begin(), utf8.end()));
#else
    return fs::u8path(utf8);
#endif
}

std::string U8(const fs::path& p) {
#if defined(__cpp_char8_t)
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
#else
    return p.u8string();
#endif
}

std::string Rel(const fs::path& p, const fs::path& base) {
    std::error_code ec;
    fs::path r = fs::relative(p, base, ec);
    if (ec || r.empty()) r = p;
    std::string s = U8(r);
    for (char& c : s) {
        if (c == '\\') c = '/';
    }
    return s;
}

bool ReadFile(const fs::path& p, std::string* out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    *out = ss.str();
    return true;
}

namespace {

std::string Unique() {
    static std::mt19937_64 rng(static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
    static std::mutex m;
    std::lock_guard<std::mutex> lock(m);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%08x", static_cast<unsigned>(rng()));
    return buf;
}

// rename() over an existing file; on Windows a target that is in use (a dll the game has mapped)
// is moved aside first.
bool ReplaceWith(const fs::path& tmp, const fs::path& to, std::string* error) {
    std::error_code ec;
    fs::rename(tmp, to, ec);
    if (!ec) return true;
#ifdef _WIN32
    if (Exists(to)) {
        fs::path aside = to;
        aside += "." + Unique() + ".atmt_old";
        std::error_code ec2;
        fs::rename(to, aside, ec2);
        if (!ec2) {
            fs::rename(tmp, to, ec);
            if (!ec) return true;
            fs::rename(aside, to, ec2);   // put it back
        }
    }
#endif
    std::error_code ignored;
    fs::remove(tmp, ignored);
    if (error != nullptr) *error = "cannot replace " + U8(to) + ": " + ec.message();
    return false;
}

}  // namespace

bool WriteFileAtomic(const fs::path& p, const std::string& data, std::string* error) {
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += ".tmp" + Unique();
#ifdef _WIN32
    FILE* f = _wfopen(tmp.c_str(), L"wb");
#else
    FILE* f = std::fopen(tmp.c_str(), "wb");
#endif
    if (f == nullptr) {
        if (error != nullptr) *error = "cannot write " + U8(tmp);
        return false;
    }
    // On the disk before the rename: after a crash or a power cut the file is the old one or the
    // new one, never an empty one (a journaling file system may otherwise commit the rename first).
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size() && std::fflush(f) == 0;
#ifdef _WIN32
    ok = ok && _commit(_fileno(f)) == 0;
#else
    ok = ok && fsync(fileno(f)) == 0;
#endif
    ok = std::fclose(f) == 0 && ok;
    if (!ok) {
        fs::remove(tmp, ec);
        if (error != nullptr) *error = "cannot write " + U8(tmp) + " (disk full?)";
        return false;
    }
    return ReplaceWith(tmp, p, error);
}

bool CopyFileAtomic(const fs::path& from, const fs::path& to, std::string* error) {
    std::error_code ec;
    if (to.has_parent_path()) fs::create_directories(to.parent_path(), ec);
    fs::path tmp = to;
    tmp += ".tmp" + Unique();
    fs::copy_file(from, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error != nullptr) *error = "cannot copy " + U8(from) + " -> " + U8(tmp) + ": " + ec.message();
        fs::remove(tmp, ec);
        return false;
    }
    // on the disk before the rename, as in WriteFileAtomic
#ifdef _WIN32
    FILE* f = _wfopen(tmp.c_str(), L"r+b");
#else
    FILE* f = std::fopen(tmp.c_str(), "r+b");
#endif
    if (f != nullptr) {
#ifdef _WIN32
        _commit(_fileno(f));
#else
        fsync(fileno(f));
#endif
        std::fclose(f);
    }
    return ReplaceWith(tmp, to, error);
}

void CleanupOldFiles(const fs::path& dir, bool recursive) {
    std::error_code ec;
    if (!IsDir(dir)) return;
    std::vector<fs::path> old;
    auto consider = [&](const fs::directory_entry& e) {
        std::error_code ec2;
        if (e.is_regular_file(ec2) && EndsWith(U8(e.path().filename()), ".atmt_old")) old.push_back(e.path());
    };
    if (recursive) {
        for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it != end; it.increment(ec)) {
            consider(*it);
        }
    } else {
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) consider(*it);
    }
    for (const fs::path& p : old) fs::remove(p, ec);
}

bool RemoveFile(const fs::path& p, std::string* error) {
    std::error_code ec;
    if (!fs::exists(p, ec)) return true;
    fs::remove(p, ec);
#ifdef _WIN32
    if (ec) {   // in use: move it aside, CleanupOldFiles takes it later
        fs::path aside = p;
        aside += "." + Unique() + ".atmt_old";
        std::error_code ec2;
        fs::rename(p, aside, ec2);
        if (!ec2) return true;
    }
#endif
    if (ec) {
        if (error != nullptr) *error = "cannot remove " + U8(p) + ": " + ec.message();
        return false;
    }
    return true;
}

bool CopyTree(const fs::path& from, const fs::path& to, std::string* error) {
    std::error_code ec;
    fs::create_directories(to, ec);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        if (error != nullptr) *error = "cannot copy " + U8(from) + " -> " + U8(to) + ": " + ec.message();
        return false;
    }
    return true;
}

bool Exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

bool IsDir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

int64_t MTime(const fs::path& p) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    if (ec) return 0;
    return static_cast<int64_t>(t.time_since_epoch().count());
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r' || s[b - 1] == '\n')) --b;
    return s.substr(a, b - a);
}

std::string Lower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }
    return s;
}

bool IEquals(const std::string& a, const std::string& b) { return Lower(a) == Lower(b); }

bool StartsWith(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::vector<std::string> Split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t start = 0;
    for (;;) {
        const size_t pos = s.find(sep, start);
        out.push_back(s.substr(start, pos == std::string::npos ? std::string::npos : pos - start));
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    return out;
}

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(start, nl - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        out.push_back(line);
        start = nl + 1;
    }
    return out;
}

std::string Join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) out += sep;
        out += parts[i];
    }
    return out;
}

std::string TimeTag() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}

std::string IsoTimeUtc() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

int64_t UnixNow() { return static_cast<int64_t>(std::time(nullptr)); }

namespace {

std::mutex& LogMutex() {
    static std::mutex m;
    return m;
}

LogFn& Sink() {
    static LogFn fn = [](const std::string& line) {
        std::fputs(line.c_str(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    };
    return fn;
}

}  // namespace

void SetLog(LogFn fn) {
    std::lock_guard<std::mutex> lock(LogMutex());
    if (fn) Sink() = std::move(fn);
}

void Log(const std::string& line) {
    std::lock_guard<std::mutex> lock(LogMutex());
    Sink()(line);
}

void Logf(const char* format, ...) {
    char buf[2048];
    va_list args;
    va_start(args, format);
    std::vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    Log(buf);
}

}  // namespace atmt
