// bgm_map.cpp - FalcomBGMInfo's song map: the parser and the lookup (bgm_map.h).
#include "bgm_map.h"

#include <algorithm>
#include <cstring>

namespace bgm {

namespace {

// Long enough for any path the game opens; a longer one is simply not a song.
constexpr size_t kMaxPath = 520;

char Fold(unsigned c) {
    if (c == '/') return '\\';
    if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
    return c < 0x80 ? static_cast<char>(c) : '\x7f';   // the keys are ASCII: nothing else matches
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
    return s.substr(a, b - a);
}

// A yaml scalar as the map writes them: "double quoted" (with \" and \\), 'single quoted' (with ''),
// or plain.
std::string Unquote(const std::string& raw) {
    const std::string s = Trim(raw);
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"') {
        std::string out;
        for (size_t i = 1; i + 1 < s.size(); ++i) {
            if (s[i] == '\\' && i + 2 < s.size()) ++i;
            out.push_back(s[i]);
        }
        return out;
    }
    if (s.size() >= 2 && s.front() == '\'' && s.back() == '\'') {
        std::string out;
        for (size_t i = 1; i + 1 < s.size(); ++i) {
            out.push_back(s[i]);
            if (s[i] == '\'' && s[i + 1] == '\'') ++i;
        }
        return out;
    }
    return s;
}

std::vector<std::string> Split(const std::string& s, char delimiter) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        const size_t at = s.find(delimiter, start);
        parts.push_back(s.substr(start, at == std::string::npos ? std::string::npos : at - start));
        if (at == std::string::npos) return parts;
        start = at + 1;
    }
}

bool EndsWithAudio(const char* folded, size_t n) {
    auto ends = [&](const char* ext) {
        const size_t e = std::strlen(ext);
        return n >= e && std::memcmp(folded + n - e, ext, e) == 0;
    };
    return ends(".wav") || ends(".ogg") || ends(".opus");
}

template <typename Char>
bool IsAudio(const Char* path) {
    if (path == nullptr) return false;
    size_t n = 0;
    while (path[n] != 0) ++n;
    if (n < 5) return false;
    char tail[6];
    const size_t take = n < 5 ? n : 5;
    for (size_t i = 0; i < take; ++i) tail[i] = Fold(static_cast<unsigned>(path[n - take + i]));
    return EndsWithAudio(tail, take);
}

template <typename Char>
size_t FoldPath(const Char* path, char* out) {
    if (path == nullptr) return 0;
    size_t n = 0;
    while (path[n] != 0) {
        if (n + 1 >= kMaxPath) return 0;
        out[n] = Fold(static_cast<unsigned>(path[n]));
        ++n;
    }
    out[n] = '\0';
    return n;
}

}  // namespace

size_t Map::Load(const char* text, size_t size, size_t* skipped) {
    songs_.clear();
    size_t bad = 0;
    size_t start = 0;
    while (start < size) {
        size_t end = start;
        while (end < size && text[end] != '\n') ++end;
        const std::string line = Trim(std::string(text + start, end - start));
        start = end + 1;
        if (line.empty() || line[0] == '#') continue;

        // "key: value" - the key is a path, so the first ": " ends it (a drive letter's "C:\" is not
        // followed by a space).
        const size_t colon = line.find(": ");
        if (colon == std::string::npos) {
            ++bad;
            continue;
        }
        const std::string key = Unquote(line.substr(0, colon));
        const std::vector<std::string> parts = Split(Unquote(line.substr(colon + 2)), '|');
        if (key.empty() || parts.empty() || Trim(parts[0]).empty()) {
            ++bad;
            continue;
        }

        Song song;
        for (char c : key) song.path.push_back(Fold(static_cast<unsigned char>(c)));
        const size_t slash = song.path.rfind('\\');
        song.file = slash == std::string::npos ? song.path : song.path.substr(slash + 1);
        song.title = Trim(parts[0]);
        if (parts.size() > 1) song.japanese = Trim(parts[1]);
        if (parts.size() > 2) song.disc = Trim(parts[2]);
        if (parts.size() > 3) song.track = Trim(parts[3]);
        if (parts.size() > 4) song.album = Trim(parts[4]);
        if (song.japanese == song.title) song.japanese.clear();

        // A path listed twice: the later line wins, as it did in FalcomBGMInfo's own std::map.
        auto same = std::find_if(songs_.begin(), songs_.end(),
                                 [&](const Song& s) { return s.path == song.path; });
        if (same != songs_.end()) *same = song;
        else songs_.push_back(song);
    }
    std::stable_sort(songs_.begin(), songs_.end(),
                     [](const Song& a, const Song& b) { return a.file < b.file; });
    if (skipped != nullptr) *skipped = bad;
    return songs_.size();
}

int Map::FindNormalized(const char* path, size_t length) const {
    const char* slash = std::strrchr(path, '\\');
    const char* file = slash != nullptr ? slash + 1 : path;
    auto it = std::lower_bound(songs_.begin(), songs_.end(), file,
                               [](const Song& s, const char* f) { return s.file.compare(f) < 0; });
    for (; it != songs_.end() && it->file.compare(file) == 0; ++it) {
        const size_t k = it->path.size();
        if (k > length) continue;
        if (std::memcmp(path + length - k, it->path.data(), k) != 0) continue;
        // a whole component: "xdata\bgm\..." is not "data\bgm\..."
        if (k < length && path[length - k - 1] != '\\') continue;
        return static_cast<int>(it - songs_.begin());
    }
    return -1;
}

int Map::Find(const char* path) const {
    char folded[kMaxPath];
    const size_t n = FoldPath(path, folded);
    return n == 0 ? -1 : FindNormalized(folded, n);
}

int Map::Find(const wchar_t* path) const {
    char folded[kMaxPath];
    const size_t n = FoldPath(path, folded);
    return n == 0 ? -1 : FindNormalized(folded, n);
}

bool IsAudioPath(const char* path) { return IsAudio(path); }
bool IsAudioPath(const wchar_t* path) { return IsAudio(path); }

}  // namespace bgm
