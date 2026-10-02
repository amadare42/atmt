// util.h - small helpers every part of manager_core uses: paths, files, text, logging.
//
// Strings are UTF-8 everywhere in the manager. Paths are std::filesystem::path; U8/Path convert at
// the edges, so a game folder with non-ASCII characters works on Windows as well as on Linux.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace atmt {

namespace fs = std::filesystem;

fs::path Path(const std::string& utf8);
std::string U8(const fs::path& p);

// A path relative to `base`, with '/' separators: what manifests, atmt_install.json and the logs use.
std::string Rel(const fs::path& p, const fs::path& base);

bool ReadFile(const fs::path& p, std::string* out);
// Writes through a temporary file next to `p` and renames it over: a reader (or a running game
// that has the old file mapped, on Linux) never sees half a file.
bool WriteFileAtomic(const fs::path& p, const std::string& data, std::string* error = nullptr);
// Copies `from` over `to` the same way. On Windows a dll the game has loaded cannot be replaced,
// but it can be renamed: the old file is moved aside to "<to>.atmt_old" first (removed later by
// CleanupOldFiles), so the copy works with the game running and takes effect on its next start.
bool CopyFileAtomic(const fs::path& from, const fs::path& to, std::string* error = nullptr);
// Removes "*.atmt_old" leftovers of CopyFileAtomic in `dir` (and below it when `recursive`); ones
// still locked stay for the next time.
void CleanupOldFiles(const fs::path& dir, bool recursive);
bool RemoveFile(const fs::path& p, std::string* error = nullptr);
// Copies a directory tree (files overwritten).
bool CopyTree(const fs::path& from, const fs::path& to, std::string* error = nullptr);

bool Exists(const fs::path& p);
bool IsDir(const fs::path& p);
int64_t MTime(const fs::path& p);   // nanoseconds-ish file time, 0 when missing; only compared

std::string Trim(const std::string& s);
std::string Lower(std::string s);
bool IEquals(const std::string& a, const std::string& b);
bool StartsWith(const std::string& s, const std::string& prefix);
bool EndsWith(const std::string& s, const std::string& suffix);
std::vector<std::string> Split(const std::string& s, char sep);
std::vector<std::string> Lines(const std::string& text);   // without "\r\n"/"\n"
std::string Join(const std::vector<std::string>& parts, const std::string& sep);

// "20261001-143005": backup folder tags and log stamps (local time).
std::string TimeTag();
// "2026-10-01T14:30:05Z"
std::string IsoTimeUtc();
int64_t UnixNow();

// Where messages of the core go: the CLI prints them, the GUI shows them in its log view. Never
// null - the default prints to stdout.
using LogFn = std::function<void(const std::string&)>;
void SetLog(LogFn fn);
void Log(const std::string& line);
void Logf(const char* format, ...);

}  // namespace atmt
