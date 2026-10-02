// falcom_pkg.cpp - see falcom_pkg.h.
#include "falcom_pkg.h"

#include <cstdio>
#include <cstring>

namespace atmt {

namespace {

uint32_t U32(const std::string& s, size_t at) {
    uint32_t v;
    std::memcpy(&v, s.data() + at, 4);   // little endian on every platform the manager runs on
    return v;
}

void PutU32(std::string* s, uint32_t v) { s->append(reinterpret_cast<const char*>(&v), 4); }

bool Fail(std::string* error, const std::string& why) {
    if (error != nullptr) *error = why;
    return false;
}

}  // namespace

bool FalcomDecompress(const std::string& blob, uint32_t size, std::string* out, std::string* error) {
    if (blob.size() < 12) return Fail(error, "type-1 stream: no header");
    const uint32_t stored = U32(blob, 4);
    const uint8_t backref = static_cast<uint8_t>(U32(blob, 8));
    if (stored > blob.size()) return Fail(error, "type-1 stream: shorter than its header says");
    std::string o;
    o.reserve(size);
    const auto* p = reinterpret_cast<const uint8_t*>(blob.data());
    size_t pos = 12;
    while (pos < stored) {
        const uint8_t byte = p[pos++];
        if (byte != backref) {
            o.push_back(static_cast<char>(byte));
            continue;
        }
        if (pos >= stored) return Fail(error, "type-1 stream: cut off");
        uint32_t offset = p[pos++];
        if (offset == backref) {
            o.push_back(static_cast<char>(backref));
            continue;
        }
        if (backref < offset) offset -= 1;
        if (pos >= stored) return Fail(error, "type-1 stream: cut off");
        const uint32_t length = p[pos++];
        if (offset == 0 || offset > o.size()) return Fail(error, "type-1 stream: a back reference before the start");
        for (uint32_t i = 0; i < length; ++i) o.push_back(o[o.size() - offset]);
    }
    if (o.size() != size) {
        return Fail(error, "type-1 stream gave " + std::to_string(o.size()) + " bytes, expected " + std::to_string(size));
    }
    *out = std::move(o);
    return true;
}

std::string FalcomCompress(const std::string& data) {
    size_t counts[256] = {};
    for (unsigned char c : data) ++counts[c];
    int backref = 0;
    for (int v = 1; v < 256; ++v) {
        if (counts[v] < counts[backref]) backref = v;   // the first of the least used values
    }
    std::string escaped;
    escaped.reserve(data.size() + counts[backref]);
    for (unsigned char c : data) {
        escaped.push_back(static_cast<char>(c));
        if (c == backref) escaped.push_back(static_cast<char>(c));
    }
    std::string out;
    out.reserve(12 + escaped.size());
    PutU32(&out, static_cast<uint32_t>(data.size()));
    PutU32(&out, static_cast<uint32_t>(12 + escaped.size()));
    PutU32(&out, static_cast<uint32_t>(backref));
    out += escaped;
    return out;
}

std::string Pkg::Entry::Name() const {
    const size_t nul = name.find('\0');
    return nul == std::string::npos ? name : name.substr(0, nul);
}

bool Pkg::Parse(const std::string& raw, std::string* error) {
    entries.clear();
    if (raw.size() < 8) return Fail(error, "not a package (too short)");
    unknown = U32(raw, 0);
    const uint32_t count = U32(raw, 4);
    if (static_cast<uint64_t>(count) * 0x50 + 8 > raw.size()) return Fail(error, "not a package (bad entry count)");
    entries.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t at = 8 + static_cast<size_t>(i) * 0x50;
        Entry e;
        e.name = raw.substr(at, 0x40);
        e.size = U32(raw, at + 0x40);
        const uint32_t stored = U32(raw, at + 0x44);
        const uint32_t offset = U32(raw, at + 0x48);
        e.flags = U32(raw, at + 0x4c);
        if (static_cast<uint64_t>(offset) + stored > raw.size()) return Fail(error, "package entry " + e.Name() + " is outside the file");
        e.stored = raw.substr(offset, stored);
        entries.push_back(std::move(e));
    }
    return true;
}

std::vector<std::string> Pkg::Names() const {
    std::vector<std::string> out;
    for (const Entry& e : entries) out.push_back(e.Name());
    return out;
}

bool Pkg::Has(const std::string& name) const {
    for (const Entry& e : entries) {
        if (e.Name() == name) return true;
    }
    return false;
}

bool Pkg::Read(const std::string& name, std::string* out, std::string* error) const {
    for (const Entry& e : entries) {
        if (e.Name() != name) continue;
        std::string blob = e.stored;
        if ((e.flags & 2) != 0) blob = blob.size() >= 4 ? blob.substr(4) : std::string();
        if ((e.flags & ~3u) != 0) {
            char flags[16];
            std::snprintf(flags, sizeof(flags), "%#x", e.flags);
            return Fail(error, name + ": package compression flags " + flags + " not supported");
        }
        if ((e.flags & 1) != 0) return FalcomDecompress(blob, e.size, out, error);
        *out = std::move(blob);
        return true;
    }
    return Fail(error, name + ": not in the package");
}

bool Pkg::Replace(const std::string& name, const std::string& data) {
    for (Entry& e : entries) {
        if (e.Name() != name) continue;
        e.size = static_cast<uint32_t>(data.size());
        e.flags = 1;
        e.stored = FalcomCompress(data);
        return true;
    }
    return false;
}

std::string Pkg::Build() const {
    std::string out;
    size_t total = 8 + 0x50 * entries.size();
    for (const Entry& e : entries) total += e.stored.size();
    out.reserve(total);
    PutU32(&out, unknown);
    PutU32(&out, static_cast<uint32_t>(entries.size()));
    uint32_t offset = static_cast<uint32_t>(8 + 0x50 * entries.size());
    for (const Entry& e : entries) {
        std::string name = e.name;
        name.resize(0x40, '\0');
        out += name;
        PutU32(&out, e.size);
        PutU32(&out, static_cast<uint32_t>(e.stored.size()));
        PutU32(&out, offset);
        PutU32(&out, e.flags);
        offset += static_cast<uint32_t>(e.stored.size());
    }
    for (const Entry& e : entries) out += e.stored;
    return out;
}

}  // namespace atmt
