// p3a.cpp - see p3a.h. zstd (and its xxhash) come from third_party/zstd.
#include "p3a.h"

#include <cstring>
#include <fstream>

#include "xxhash.h"
#include "zstd.h"

namespace atmt {

namespace {

bool Fail(std::string* error, const std::string& why) {
    if (error != nullptr) *error = why;
    return false;
}

template <typename T>
T Get(const char* p) {
    T v;
    std::memcpy(&v, p, sizeof(T));
    return v;
}

template <typename T>
void Put(std::string* s, T v) {
    s->append(reinterpret_cast<const char*>(&v), sizeof(T));
}

}  // namespace

uint64_t Xxh64(const void* data, size_t size) { return XXH64(data, size, 0); }

bool Lz4BlockDecompress(const std::string& in, size_t size, std::string* out) {
    std::string o;
    o.reserve(size);
    const auto* p = reinterpret_cast<const uint8_t*>(in.data());
    const size_t n = in.size();
    size_t i = 0;
    while (i < n) {
        const uint8_t token = p[i++];
        size_t lit = token >> 4;
        if (lit == 15) {
            uint8_t b;
            do {
                if (i >= n) return false;
                b = p[i++];
                lit += b;
            } while (b == 255);
        }
        if (lit > n - i || o.size() + lit > size) return false;
        o.append(reinterpret_cast<const char*>(p + i), lit);
        i += lit;
        if (i >= n) break;   // the last sequence has only literals
        if (n - i < 2) return false;
        const size_t offset = p[i] | (static_cast<size_t>(p[i + 1]) << 8);
        i += 2;
        size_t len = token & 15;
        if (len == 15) {
            uint8_t b;
            do {
                if (i >= n) return false;
                b = p[i++];
                len += b;
            } while (b == 255);
        }
        len += 4;
        if (offset == 0 || offset > o.size() || o.size() + len > size) return false;
        const size_t from = o.size() - offset;
        for (size_t k = 0; k < len; ++k) o.push_back(o[from + k]);
    }
    if (o.size() != size) return false;
    *out = std::move(o);
    return true;
}

bool ZstdDecompress(const std::string& in, size_t size, std::string* out) {
    std::string o(size, '\0');
    const size_t got = ZSTD_decompress(o.empty() ? nullptr : &o[0], size, in.data(), in.size());
    if (ZSTD_isError(got) || got != size) return false;
    *out = std::move(o);
    return true;
}

std::string ZstdCompress(const std::string& in, int level) {
    std::string o(ZSTD_compressBound(in.size()), '\0');
    const size_t got = ZSTD_compress(&o[0], o.size(), in.data(), in.size(), level);
    if (ZSTD_isError(got)) return std::string();
    o.resize(got);
    return o;
}

std::string P3aReader::Key(const std::string& name) {
    std::string k = name;
    for (char& c : k) {
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return k;
}

bool P3aReader::Open(const fs::path& path, std::string* error) {
    path_ = path;
    entries_.clear();
    std::ifstream f(path, std::ios::binary);
    if (!f) return Fail(error, U8(path.filename()) + ": cannot open");
    char header[0x20];
    if (!f.read(header, sizeof(header)) || std::memcmp(header, "PH3ARCV\0", 8) != 0) {
        return Fail(error, U8(path.filename()) + ": not a .p3a archive");
    }
    flags_ = Get<uint32_t>(header + 8);
    const uint32_t version = Get<uint32_t>(header + 12);
    const uint64_t count = Get<uint64_t>(header + 16);
    uint64_t start = 0x20, entry_size = 296;
    if (version >= 1200) {
        char ext[16];
        if (!f.read(ext, sizeof(ext))) return Fail(error, U8(path.filename()) + ": cut off");
        start = 0x20 + Get<uint32_t>(ext + 8);
        entry_size = Get<uint32_t>(ext + 12);
    }
    if (entry_size < 288 || count > 10000000) return Fail(error, U8(path.filename()) + ": a table this app cannot read");
    std::string table(static_cast<size_t>(count * entry_size), '\0');
    f.seekg(static_cast<std::streamoff>(start));
    if (!table.empty() && !f.read(&table[0], static_cast<std::streamsize>(table.size()))) {
        return Fail(error, U8(path.filename()) + ": the table is cut off");
    }
    for (uint64_t i = 0; i < count; ++i) {
        const char* e = table.data() + i * entry_size;
        Entry entry;
        entry.name.assign(e, strnlen(e, 256));
        entry.kind = Get<uint64_t>(e + 256);
        entry.stored = Get<uint64_t>(e + 264);
        entry.size = Get<uint64_t>(e + 272);
        entry.offset = Get<uint64_t>(e + 280);
        entries_[Key(entry.name)] = entry;
    }
    return true;
}

bool P3aReader::Has(const std::string& name) const { return entries_.count(Key(name)) != 0; }

bool P3aReader::Read(const std::string& name, std::string* out, std::string* error) const {
    const auto it = entries_.find(Key(name));
    if (it == entries_.end()) return Fail(error, name + ": not in " + U8(path_.filename()));
    const Entry& e = it->second;
    const std::string where = U8(path_.filename()) + ": " + name;
    if (e.kind > 2) return Fail(error, where + ": compression " + std::to_string(e.kind) + " not supported");
    if (e.stored > (uint64_t(1) << 31) || e.size > (uint64_t(1) << 31)) return Fail(error, where + ": too large");
    std::ifstream f(path_, std::ios::binary);
    std::string blob(static_cast<size_t>(e.stored), '\0');
    f.seekg(static_cast<std::streamoff>(e.offset));
    if (!f || (!blob.empty() && !f.read(&blob[0], static_cast<std::streamsize>(blob.size())))) {
        return Fail(error, where + ": cut off");
    }
    if (e.kind == 0) {
        *out = std::move(blob);
        return true;
    }
    const bool ok = e.kind == 2 ? ZstdDecompress(blob, static_cast<size_t>(e.size), out)
                                : Lz4BlockDecompress(blob, static_cast<size_t>(e.size), out);
    return ok ? true : Fail(error, where + (e.kind == 2 ? ": damaged zstd data" : ": damaged lz4 data"));
}

bool WriteP3a(const fs::path& path, const std::vector<std::pair<std::string, std::string>>& files, int zstd_level,
              std::string* error) {
    std::string header("PH3ARCV\0", 8);
    Put<uint32_t>(&header, 0);
    Put<uint32_t>(&header, 1100);
    Put<uint64_t>(&header, files.size());
    Put<uint64_t>(&header, Xxh64(header.data(), header.size()));
    std::vector<std::string> blobs;
    for (const auto& f : files) {
        if (f.first.size() >= 256) return Fail(error, f.first + ": the name is too long for a .p3a");
        if (zstd_level > 0) {
            blobs.push_back(ZstdCompress(f.second, zstd_level));
            if (blobs.back().empty() && !f.second.empty()) return Fail(error, f.first + ": zstd compression failed");
        } else {
            blobs.push_back(f.second);
        }
    }
    uint64_t offset = 0x20 + 296 * files.size();
    std::string table, body;
    for (size_t i = 0; i < files.size(); ++i) {
        const uint64_t pad = (16 - offset % 16) % 16;
        body.append(static_cast<size_t>(pad), '\0');
        offset += pad;
        std::string name = files[i].first;
        name.resize(256, '\0');
        table += name;
        Put<uint64_t>(&table, zstd_level > 0 ? 2 : 0);
        Put<uint64_t>(&table, blobs[i].size());
        Put<uint64_t>(&table, files[i].second.size());
        Put<uint64_t>(&table, offset);
        Put<uint64_t>(&table, Xxh64(blobs[i].data(), blobs[i].size()));
        body += blobs[i];
        offset += blobs[i].size();
    }
    return WriteFileAtomic(path, header + table + body, error);
}

}  // namespace atmt
