// falcom_pkg.h - the game's .pkg packages (data/asset/D3D11[_us]/*.pkg) and Falcom's type-1
// compression inside them (docs/ENGINE_NOTES.md, "Asset formats").
//
//   u32 unknown, u32 count, count x { char name[0x40]; u32 size, stored_size, offset, flags }, data
//   flags: 1 type-1 compression, 2 a crc32 prefix, 4 lz4, 8 lzma, 0x10 zstd
//
// The game ships type 1, which is all this reads or writes; an entry with other flags cannot be
// read (Read fails), but is carried through Build unchanged.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace atmt {

// Type 1: u32 size, u32 stored_size (with this header), u32 backref byte, then the stream: a byte
// that is not the backref byte is a literal; the backref byte is followed by an offset (the backref
// byte itself: a literal backref byte; above it: one less) and a length to copy from that far back.
bool FalcomDecompress(const std::string& blob, uint32_t size, std::string* out, std::string* error = nullptr);
// Type 1 with literals only (the archive around it is compressed anyway): the least used byte value
// is the backref byte, and each of its occurrences is doubled.
std::string FalcomCompress(const std::string& data);

class Pkg {
public:
    struct Entry {
        std::string name;      // the 0x40 raw name bytes, kept as they were
        uint32_t size = 0;
        uint32_t flags = 0;
        std::string stored;    // the stored bytes
        std::string Name() const;   // up to the first NUL
    };

    bool Parse(const std::string& raw, std::string* error = nullptr);
    std::vector<std::string> Names() const;
    bool Has(const std::string& name) const;
    bool Read(const std::string& name, std::string* out, std::string* error = nullptr) const;
    // Replaces an entry's data (stored type-1 compressed, flags 1); false if there is no such entry.
    bool Replace(const std::string& name, const std::string& data);
    std::string Build() const;

    uint32_t unknown = 0;
    std::vector<Entry> entries;
};

}  // namespace atmt
