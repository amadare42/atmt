// ini.h - the tiny ini reader (and writer) used by the loader, the mods and the manager.
//
// Deliberately minimal and header-only: mods should not have to link anything to
// read their own settings, and the format we need is "key=value" inside "[section]"
// with ';' or '#' comments. Unknown keys are ignored, so an ini written by another
// version is harmless.
#ifndef ATMT_INI_H
#define ATMT_INI_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include <cstddef>
#include <string>
#include <vector>

#ifdef _WIN32
#include <io.h>   // _commit, _fileno
// Declared here rather than by including <windows.h>, which would impose its macros (min/max,
// WIN32_LEAN_AND_MEAN or not) on every file that includes this header. Same types as windows.h's,
// so both declarations can meet in one file.
extern "C" __declspec(dllimport) int __stdcall MoveFileExW(const wchar_t* existing, const wchar_t* replacement,
                                                           unsigned long flags);
extern "C" __declspec(dllimport) void __stdcall Sleep(unsigned long ms);
#else
#include <unistd.h>   // fsync
#endif

namespace atmt_ini {

#ifndef _WIN32
// The manager (manager/) also builds for Linux, where these MSVC names do not exist. Kept inside the
// namespace, so nothing leaks into the code that includes this header.
inline int _stricmp(const char* a, const char* b) {
    for (;; ++a, ++b) {
        const int ca = (*a >= 'A' && *a <= 'Z') ? *a + 32 : static_cast<unsigned char>(*a);
        const int cb = (*b >= 'A' && *b <= 'Z') ? *b + 32 : static_cast<unsigned char>(*b);
        if (ca != cb || ca == 0) return ca - cb;
    }
}
inline int _strnicmp(const char* a, const char* b, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const int ca = (a[i] >= 'A' && a[i] <= 'Z') ? a[i] + 32 : static_cast<unsigned char>(a[i]);
        const int cb = (b[i] >= 'A' && b[i] <= 'Z') ? b[i] + 32 : static_cast<unsigned char>(b[i]);
        if (ca != cb || ca == 0) return ca - cb;
    }
    return 0;
}
template <typename... Args>
inline int _snprintf(char* out, size_t size, const char* format, Args... args) {
    return snprintf(out, size, format, args...);
}
// wchar_t is UTF-32 here: the path is encoded to UTF-8, which is what the file system takes.
inline std::string NarrowPath(const wchar_t* path) {
    std::string p;
    for (const wchar_t* w = path; *w != L'\0'; ++w) {
        const unsigned long c = static_cast<unsigned long>(*w);
        if (c < 0x80) {
            p += static_cast<char>(c);
        } else if (c < 0x800) {
            p += static_cast<char>(0xC0 | (c >> 6));
            p += static_cast<char>(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            p += static_cast<char>(0xE0 | (c >> 12));
            p += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            p += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            p += static_cast<char>(0xF0 | (c >> 18));
            p += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
            p += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            p += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return p;
}
inline FILE* _wfopen(const wchar_t* path, const wchar_t* mode) {
    std::string m;
    for (const wchar_t* w = mode; *w != L'\0'; ++w) m += static_cast<char>(*w);
    return fopen(NarrowPath(path).c_str(), m.c_str());
}
#endif

// Replaces the file at `path` with `data` as a whole: written to <path>.tmp, flushed to the disk,
// then renamed over it. A crash or a power cut (a Steam Deck's battery) leaves the old file or the
// new one, never a half-written or empty one. On Windows the rename can fail while another process
// has the file open without delete sharing (a reader at that very moment): then it is written in
// place, as before.
inline bool ReplaceFile(const wchar_t* path, const std::string& data) {
    if (path == nullptr) return false;
    const std::wstring tmp = std::wstring(path) + L".tmp";
    FILE* f = _wfopen(tmp.c_str(), L"wb");
    if (f != nullptr) {
        bool ok = fwrite(data.data(), 1, data.size(), f) == data.size() && fflush(f) == 0;
#ifdef _WIN32
        ok = ok && _commit(_fileno(f)) == 0;
#else
        ok = ok && fsync(fileno(f)) == 0;
#endif
        ok = (fclose(f) == 0) && ok;
        if (ok) {
#ifdef _WIN32
            // MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH
            for (int attempt = 0; attempt < 3; ++attempt) {
                if (::MoveFileExW(tmp.c_str(), path, 0x1 | 0x8) != 0) return true;
                ::Sleep(15);
            }
#else
            if (rename(NarrowPath(tmp.c_str()).c_str(), NarrowPath(path).c_str()) == 0) return true;
#endif
        }
#ifdef _WIN32
        _wremove(tmp.c_str());
#else
        remove(NarrowPath(tmp.c_str()).c_str());
#endif
    }
    f = _wfopen(path, L"wb");
    if (f == nullptr) return false;
    const bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
    return (fclose(f) == 0) && ok;
}

inline bool Bool(const char* v, bool def) {
    if (v == nullptr) return def;
    if (_stricmp(v, "true") == 0 || _stricmp(v, "1") == 0 || _stricmp(v, "yes") == 0
        || _stricmp(v, "on") == 0) {
        return true;
    }
    if (_stricmp(v, "false") == 0 || _stricmp(v, "0") == 0 || _stricmp(v, "no") == 0
        || _stricmp(v, "off") == 0) {
        return false;
    }
    return def;
}

inline int Int(const char* v, int def) {
    if (v == nullptr || *v == '\0') return def;
    return static_cast<int>(strtol(v, nullptr, 0));
}

// Where an inline comment starts in `v` ("5   ; why" -> the ';'), or NULL. Only a ';'
// preceded by a space or a tab counts, so a value may still contain one ("a;b").
inline char* InlineComment(char* v) {
    for (char* p = v; *p != '\0'; ++p) {
        if (*p == ';' && p > v && (p[-1] == ' ' || p[-1] == '\t')) return p;
    }
    return nullptr;
}

// Reads `key` from `section` of the ini at `path`. Returns def when the file, the
// section or the key is missing. Case-insensitive for both section and key. A NULL
// or empty section means the keys before the first [section] header. An inline
// comment (" ; ...") is not part of the value.
inline bool Read(const wchar_t* path, const char* section, const char* key,
                 char* out, size_t out_size, const char* def = "") {
    if (path == nullptr || out == nullptr || out_size == 0) return false;
    if (def != nullptr) {
        _snprintf(out, out_size, "%s", def);
    } else {
        out[0] = '\0';
    }
    FILE* f = _wfopen(path, L"rb");
    if (f == nullptr) return false;

    bool in_section = section == nullptr || *section == '\0';
    bool found = false;
    char line[1024];
    while (fgets(line, sizeof(line), f) != nullptr) {
        char* s = line;
        while (*s == ' ' || *s == '\t') ++s;
        if (*s == ';' || *s == '#' || *s == '\r' || *s == '\n') continue;
        if (*s == '[') {
            char* end = strchr(s, ']');
            if (end == nullptr) continue;
            *end = '\0';
            in_section = section != nullptr && *section != '\0' && _stricmp(s + 1, section) == 0;
            continue;
        }
        if (!in_section) continue;
        char* eq = strchr(s, '=');
        if (eq == nullptr) continue;
        *eq = '\0';
        char* k = s;
        size_t klen = strlen(k);
        while (klen > 0 && (k[klen - 1] == ' ' || k[klen - 1] == '\t')) k[--klen] = '\0';
        if (_stricmp(k, key) != 0) continue;
        char* v = eq + 1;
        while (*v == ' ' || *v == '\t') ++v;
        if (char* comment = InlineComment(v)) *comment = '\0';
        size_t vlen = strlen(v);
        while (vlen > 0 && (v[vlen - 1] == '\r' || v[vlen - 1] == '\n' || v[vlen - 1] == ' '
                            || v[vlen - 1] == '\t')) {
            v[--vlen] = '\0';
        }
        _snprintf(out, out_size, "%s", v);
        found = true;
        break;
    }
    fclose(f);
    return found;
}

inline bool ReadBool(const wchar_t* path, const char* section, const char* key, bool def) {
    char buf[64];
    if (!Read(path, section, key, buf, sizeof(buf), nullptr)) return def;
    return Bool(buf, def);
}

inline int ReadInt(const wchar_t* path, const char* section, const char* key, int def) {
    char buf[64];
    if (!Read(path, section, key, buf, sizeof(buf), nullptr)) return def;
    return Int(buf, def);
}

// Writes `path` with `content` only if it does not exist yet, so a user can see and
// edit all available settings instead of having to read the docs.
inline void WriteDefaultIfMissing(const wchar_t* path, const char* content) {
    if (path == nullptr) return;
    if (FILE* existing = _wfopen(path, L"rb")) {
        fclose(existing);
        return;
    }
    ReplaceFile(path, content);
}

// ---------------------------------------------------------------- writing values back
// One key to write: its value, and the comment that goes above it if the key has to
// be added (NULL/empty = none; several lines are fine).
struct Entry {
    std::string key;
    std::string value;
    std::string comment;
};

namespace detail {

// If `line` is `key=...` (whitespace and case insensitive), replaces the value in
// place - an inline comment and the line ending stay exactly as they were - and
// returns true.
inline bool ReplaceValue(std::string& line, const std::string& key, const std::string& value) {
    size_t p = 0;
    while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
    if (p >= line.size() || line[p] == ';' || line[p] == '#' || line[p] == '[') return false;
    const size_t eq = line.find('=', p);
    if (eq == std::string::npos) return false;
    size_t key_end = eq;
    while (key_end > p && (line[key_end - 1] == ' ' || line[key_end - 1] == '\t')) --key_end;
    if (key_end - p != key.size() || _strnicmp(line.c_str() + p, key.c_str(), key.size()) != 0) {
        return false;
    }
    size_t start = eq + 1;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
    size_t end = line.find_first_of("\r\n", start);
    if (end == std::string::npos) end = line.size();
    for (size_t i = start + 1; i < end; ++i) {   // same rule as InlineComment
        if (line[i] == ';' && (line[i - 1] == ' ' || line[i - 1] == '\t')) {
            end = i;
            break;
        }
    }
    while (end > start && (line[end - 1] == ' ' || line[end - 1] == '\t')) --end;
    line = line.substr(0, start) + value + line.substr(end);
    return true;
}

// "[General]" -> "General"; empty when the line is not a section header.
inline std::string SectionOf(const std::string& line) {
    size_t p = 0;
    while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
    if (p >= line.size() || line[p] != '[') return std::string();
    const size_t end = line.find(']', p);
    if (end == std::string::npos) return std::string();
    return line.substr(p + 1, end - p - 1);
}

inline bool IsBlank(const std::string& line) {
    for (char c : line) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') return false;
    }
    return true;
}

}  // namespace detail

// Writes `entries` into `section` of the ini at `path` (NULL/empty section = before
// the first header), creating the file if needed. A key already in that section keeps
// its line, its inline comment and its position - only the value changes. A missing
// key is added after the section's last key (and the comment lines directly under
// it, up to the first blank line - a comment block further down usually introduces
// the next section), with its comment above it; a missing section is appended to the
// file. Never a rewrite from scratch: the comments are most of what a shipped ini is.
inline bool WriteValues(const wchar_t* path, const char* section, const Entry* entries,
                        size_t count) {
    if (path == nullptr) return false;
    std::vector<std::string> lines;
    std::string eol = "\r\n";
    if (FILE* f = _wfopen(path, L"rb")) {
        std::string content;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) content.append(buf, n);
        fclose(f);
        const size_t first_nl = content.find('\n');
        if (first_nl != std::string::npos && (first_nl == 0 || content[first_nl - 1] != '\r')) {
            eol = "\n";
        }
        size_t start = 0;
        for (size_t i = 0; i < content.size(); ++i) {
            if (content[i] == '\n') {
                lines.push_back(content.substr(start, i - start + 1));
                start = i + 1;
            }
        }
        if (start < content.size()) lines.push_back(content.substr(start));
    }

    const bool top = section == nullptr || *section == '\0';
    std::vector<bool> found(count, false);
    bool in_section = top;
    bool section_seen = top;
    size_t insert_at = 0;       // after the section's last key and the comments right under it
    bool after_key = false;     // still in the lines directly under that key
    for (size_t li = 0; li < lines.size(); ++li) {
        std::string& line = lines[li];
        const std::string header = detail::SectionOf(line);
        if (!header.empty()) {
            in_section = !top && _stricmp(header.c_str(), section) == 0;
            after_key = false;
            if (in_section) {
                section_seen = true;
                insert_at = li + 1;
            }
            continue;
        }
        if (!in_section) continue;
        if (detail::IsBlank(line)) {
            after_key = false;
        } else if (line.find('=') != std::string::npos && line.find_first_not_of(" \t") != line.find(';')
                   && line.find_first_not_of(" \t") != line.find('#')) {
            insert_at = li + 1;
            after_key = true;
        } else if (after_key) {
            insert_at = li + 1;
        }
        for (size_t i = 0; i < count; ++i) {
            if (!found[i] && detail::ReplaceValue(line, entries[i].key, entries[i].value)) {
                found[i] = true;
                break;
            }
        }
    }

    std::vector<std::string> added;
    for (size_t i = 0; i < count; ++i) {
        if (found[i]) continue;
        const std::string& c = entries[i].comment;
        size_t start = 0;
        while (start < c.size()) {
            size_t nl = c.find('\n', start);
            if (nl == std::string::npos) nl = c.size();
            added.push_back("; " + c.substr(start, nl - start) + eol);
            start = nl + 1;
        }
        added.push_back(entries[i].key + "=" + entries[i].value + eol);
    }
    if (!added.empty()) {
        // a last line without its line ending would swallow the first added one
        if (!lines.empty() && lines.back().find('\n') == std::string::npos) lines.back() += eol;
        if (!section_seen) {
            if (!lines.empty() && !detail::IsBlank(lines.back())) lines.push_back(eol);
            lines.push_back("[" + std::string(section) + "]" + eol);
            insert_at = lines.size();
        } else if (insert_at > 0 && lines[insert_at - 1].find('\n') == std::string::npos) {
            lines[insert_at - 1] += eol;
        }
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insert_at), added.begin(),
                     added.end());
    }

    std::string data;
    for (const std::string& line : lines) data += line;
    return ReplaceFile(path, data);
}

}  // namespace atmt_ini

#endif  // ATMT_INI_H
