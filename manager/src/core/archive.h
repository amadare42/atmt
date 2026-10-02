// archive.h - unpacking payload-<ver>.tar.gz and SenPatcher's release zip (docs/MANAGER.md).
//
// A self-contained inflate (RFC 1951, after zlib's puff.c), a tar reader (ustar, pax and GNU
// long names - what both GNU tar and Windows' bsdtar write) and a zip reader (stored and deflated
// entries, no zip64), so the manager needs no zlib.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "util.h"

namespace atmt {

// Raw deflate data -> bytes.
bool Inflate(const std::string& deflated, std::string* out, std::string* error = nullptr);
// A .gz file's contents (one member; its CRC and length are checked) -> bytes.
bool Gunzip(const std::string& gz, std::string* out, std::string* error = nullptr);
// Extracts a tar archive's regular files and folders below `dest`. Refuses entries that would land
// outside it (absolute paths, "..") and links of any kind.
bool ExtractTar(const std::string& tar, const fs::path& dest, std::string* error = nullptr);
bool ExtractTarGz(const fs::path& archive, const fs::path& dest, std::string* error = nullptr);

// One file of a zip archive, from its central directory.
struct ZipEntry {
    std::string name;            // as stored: '/' separated, a folder ends with '/'
    uint16_t method = 0;         // 0 stored, 8 deflated (others are refused when read)
    uint32_t crc = 0;
    uint64_t compressed = 0;
    uint64_t size = 0;
    uint64_t local_offset = 0;   // its local header
    bool IsDir() const { return !name.empty() && name.back() == '/'; }
};
// The entries of a zip archive (its central directory).
bool ListZip(const std::string& zip, std::vector<ZipEntry>* out, std::string* error = nullptr);
// One entry's contents; its size and CRC are checked.
bool ReadZipEntry(const std::string& zip, const ZipEntry& entry, std::string* out, std::string* error = nullptr);

uint32_t Crc32(const std::string& data);

}  // namespace atmt
