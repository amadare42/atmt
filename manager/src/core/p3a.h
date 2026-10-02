// p3a.h - SenPatcher's mod archives (<game>/mods/*.p3a; docs/ENGINE_NOTES.md,
// "Asset formats").
//
//   char magic[8] "PH3ARCV\0"; u32 flags; u32 version (1100 | 1200); u64 count; u64 xxh64(bytes 0..23)
//   v1200: u64 xxh64(next 8 bytes); u32 ext_size (16); u32 entry_size (304)
//   count x { char path[256]; u64 compression (0 none, 1 lz4, 2 zstd, 3 zstd + dictionary);
//             u64 stored_size; u64 size; u64 offset; u64 xxh64(stored bytes); [v1200: u64 xxh64(data)] }
//
// Reading opens the file once for its table and reads an entry only when asked (an HD texture pack
// is close to a gigabyte). Entries stored raw, lz4 (block) and zstd are read; a dictionary archive's
// entries (3) are not.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "util.h"

namespace atmt {

class P3aReader {
public:
    struct Entry {
        std::string name;   // as stored
        uint64_t kind = 0, stored = 0, size = 0, offset = 0;
    };

    bool Open(const fs::path& path, std::string* error = nullptr);
    // Names compare as SenPatcher compares them: case-insensitive, '\' and '/' alike.
    bool Has(const std::string& name) const;
    bool Read(const std::string& name, std::string* out, std::string* error = nullptr) const;
    const fs::path& path() const { return path_; }
    const std::map<std::string, Entry>& entries() const { return entries_; }

    static std::string Key(const std::string& name);

private:
    fs::path path_;
    uint32_t flags_ = 0;
    std::map<std::string, Entry> entries_;   // by Key(name)
};

// Writes a version-1100 archive of `files` (path inside the archive, data), in that order, through
// a temporary file. zstd_level 0 stores the data raw; above 0 it is zstd-compressed at that level.
bool WriteP3a(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& files, int zstd_level,
              std::string* error = nullptr);

// The pieces, for the tests: xxh64 (seed 0), an LZ4 block and a zstd frame decoder.
uint64_t Xxh64(const void* data, size_t size);
bool Lz4BlockDecompress(const std::string& in, size_t size, std::string* out);
bool ZstdDecompress(const std::string& in, size_t size, std::string* out);
std::string ZstdCompress(const std::string& in, int level);

}  // namespace atmt
