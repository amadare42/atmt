// archive.cpp - see archive.h.
#include "archive.h"

#include <cstring>
#include <vector>

namespace atmt {

namespace {

// ---------------------------------------------------------------- inflate (puff-style)
constexpr int kMaxBits = 15;

struct Huffman {
    short count[kMaxBits + 1];
    short symbol[288];
};

class Inflater {
public:
    Inflater(const std::string& in, std::string* out) : in_(in), out_(out) {}

    // 0 = ok, otherwise a reason.
    const char* Run() {
        int last;
        do {
            if (!Bits(1, &last)) return "truncated";
            int type;
            if (!Bits(2, &type)) return "truncated";
            const char* err = nullptr;
            if (type == 0) err = Stored();
            else if (type == 1) err = Fixed();
            else if (type == 2) err = Dynamic();
            else err = "bad block type";
            if (err != nullptr) return err;
        } while (!last);
        return nullptr;
    }

    size_t consumed() const { return pos_; }

private:
    bool Bits(int need, int* value) {
        long val = bitbuf_;
        while (bitcnt_ < need) {
            if (pos_ >= in_.size()) return false;
            val |= static_cast<long>(static_cast<uint8_t>(in_[pos_++])) << bitcnt_;
            bitcnt_ += 8;
        }
        bitbuf_ = static_cast<int>(val >> need);
        bitcnt_ -= need;
        *value = static_cast<int>(val & ((1L << need) - 1));
        return true;
    }

    const char* Stored() {
        bitbuf_ = 0;
        bitcnt_ = 0;
        if (pos_ + 4 > in_.size()) return "truncated";
        unsigned len = static_cast<uint8_t>(in_[pos_]) | (static_cast<uint8_t>(in_[pos_ + 1]) << 8);
        const unsigned nlen = static_cast<uint8_t>(in_[pos_ + 2]) | (static_cast<uint8_t>(in_[pos_ + 3]) << 8);
        pos_ += 4;
        if (len != (~nlen & 0xffff)) return "stored block length mismatch";
        if (pos_ + len > in_.size()) return "truncated";
        out_->append(in_, pos_, len);
        pos_ += len;
        return nullptr;
    }

    bool Decode(const Huffman& h, int* sym) {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len <= kMaxBits; ++len) {
            int bit;
            if (!Bits(1, &bit)) return false;
            code |= bit;
            const int count = h.count[len];
            if (code - count < first) {
                *sym = h.symbol[index + (code - first)];
                return true;
            }
            index += count;
            first += count;
            first <<= 1;
            code <<= 1;
        }
        return false;
    }

    // Returns 0 for a complete code, <0 over-subscribed, >0 incomplete.
    static int Construct(Huffman& h, const short* length, int n) {
        for (int len = 0; len <= kMaxBits; ++len) h.count[len] = 0;
        for (int s = 0; s < n; ++s) h.count[length[s]]++;
        if (h.count[0] == n) return 0;
        int left = 1;
        for (int len = 1; len <= kMaxBits; ++len) {
            left <<= 1;
            left -= h.count[len];
            if (left < 0) return left;
        }
        short offs[kMaxBits + 1];
        offs[1] = 0;
        for (int len = 1; len < kMaxBits; ++len) offs[len + 1] = static_cast<short>(offs[len] + h.count[len]);
        for (int s = 0; s < n; ++s) {
            if (length[s] != 0) h.symbol[offs[length[s]]++] = static_cast<short>(s);
        }
        return left;
    }

    const char* Codes(const Huffman& lencode, const Huffman& distcode) {
        static const short kLBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                         31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
        static const short kLExt[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                        2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
        static const short kDBase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                         193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
        static const short kDExt[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6,
                                        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
        for (;;) {
            int sym;
            if (!Decode(lencode, &sym)) return "bad literal/length code";
            if (sym < 256) {
                *out_ += static_cast<char>(sym);
                continue;
            }
            if (sym == 256) return nullptr;
            sym -= 257;
            if (sym >= 29) return "bad length symbol";
            int extra;
            if (!Bits(kLExt[sym], &extra)) return "truncated";
            const int len = kLBase[sym] + extra;
            int dsym;
            if (!Decode(distcode, &dsym) || dsym >= 30) return "bad distance code";
            if (!Bits(kDExt[dsym], &extra)) return "truncated";
            const size_t dist = static_cast<size_t>(kDBase[dsym] + extra);
            if (dist > out_->size()) return "distance too far back";
            const size_t from = out_->size() - dist;
            for (int i = 0; i < len; ++i) *out_ += (*out_)[from + static_cast<size_t>(i)];
        }
    }

    const char* Fixed() {
        struct Tables {
            Huffman lencode, distcode;
            Tables() {
                short lengths[288];
                int s = 0;
                for (; s < 144; ++s) lengths[s] = 8;
                for (; s < 256; ++s) lengths[s] = 9;
                for (; s < 280; ++s) lengths[s] = 7;
                for (; s < 288; ++s) lengths[s] = 8;
                Construct(lencode, lengths, 288);
                for (s = 0; s < 30; ++s) lengths[s] = 5;
                Construct(distcode, lengths, 30);
            }
        };
        static const Tables tables;   // thread-safe initialisation
        return Codes(tables.lencode, tables.distcode);
    }

    const char* Dynamic() {
        static const short kOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
        int nlen, ndist, ncode;
        if (!Bits(5, &nlen) || !Bits(5, &ndist) || !Bits(4, &ncode)) return "truncated";
        nlen += 257;
        ndist += 1;
        ncode += 4;
        if (nlen > 286 || ndist > 30) return "bad counts";
        short lengths[320];
        int index;
        for (index = 0; index < ncode; ++index) {
            int v;
            if (!Bits(3, &v)) return "truncated";
            lengths[kOrder[index]] = static_cast<short>(v);
        }
        for (; index < 19; ++index) lengths[kOrder[index]] = 0;
        Huffman lencode, distcode;
        if (Construct(lencode, lengths, 19) != 0) return "incomplete code-length code";
        index = 0;
        while (index < nlen + ndist) {
            int sym;
            if (!Decode(lencode, &sym)) return "bad code-length code";
            if (sym < 16) {
                lengths[index++] = static_cast<short>(sym);
                continue;
            }
            int len = 0, repeat;
            if (sym == 16) {
                if (index == 0) return "repeat with no first length";
                len = lengths[index - 1];
                if (!Bits(2, &repeat)) return "truncated";
                repeat += 3;
            } else if (sym == 17) {
                if (!Bits(3, &repeat)) return "truncated";
                repeat += 3;
            } else {
                if (!Bits(7, &repeat)) return "truncated";
                repeat += 11;
            }
            if (index + repeat > nlen + ndist) return "too many lengths";
            while (repeat-- > 0) lengths[index++] = static_cast<short>(len);
        }
        if (lengths[256] == 0) return "no end-of-block code";
        const int err = Construct(lencode, lengths, nlen);
        if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) return "bad literal/length lengths";
        const int derr = Construct(distcode, lengths + nlen, ndist);
        if (derr < 0 || (derr > 0 && ndist - distcode.count[0] != 1)) return "bad distance lengths";
        return Codes(lencode, distcode);
    }

    const std::string& in_;
    std::string* out_;
    size_t pos_ = 0;
    int bitbuf_ = 0;
    int bitcnt_ = 0;

public:
    void Seek(size_t pos) { pos_ = pos; }
};

uint64_t Octal(const char* p, size_t n) {
    uint64_t v = 0;
    size_t i = 0;
    while (i < n && (p[i] == ' ' || p[i] == '\0')) ++i;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; ++i) v = v * 8 + static_cast<uint64_t>(p[i] - '0');
    return v;
}

std::string Field(const char* p, size_t n) {
    size_t len = 0;
    while (len < n && p[len] != '\0') ++len;
    return std::string(p, len);
}

// "./a/b" -> "a/b"; empty when it would escape the destination.
std::string SafeRelative(std::string name) {
    for (char& c : name) {
        if (c == '\\') c = '/';
    }
    while (StartsWith(name, "./")) name = name.substr(2);
    if (name == ".") return std::string();
    if (name.empty() || name[0] == '/' || name.find(':') != std::string::npos) return std::string();
    for (const std::string& part : Split(name, '/')) {
        if (part == "..") return std::string();
    }
    while (!name.empty() && name.back() == '/') name.pop_back();
    return name;
}

}  // namespace

uint32_t Crc32(const std::string& data) {
    struct Table {
        uint32_t v[256];
        Table() {
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                v[i] = c;
            }
        }
    };
    static const Table t;
    const uint32_t* table = t.v;
    uint32_t crc = 0xFFFFFFFFu;
    for (unsigned char b : data) crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

bool Inflate(const std::string& deflated, std::string* out, std::string* error) {
    out->clear();
    Inflater inf(deflated, out);
    if (const char* err = inf.Run()) {
        if (error != nullptr) *error = std::string("inflate: ") + err;
        return false;
    }
    return true;
}

bool Gunzip(const std::string& gz, std::string* out, std::string* error) {
    auto fail = [&](const char* why) {
        if (error != nullptr) *error = std::string("gzip: ") + why;
        return false;
    };
    if (gz.size() < 18 || static_cast<uint8_t>(gz[0]) != 0x1f || static_cast<uint8_t>(gz[1]) != 0x8b) {
        return fail("not a gzip file");
    }
    if (gz[2] != 8) return fail("not deflate");
    const uint8_t flags = static_cast<uint8_t>(gz[3]);
    size_t pos = 10;
    if (flags & 4) {   // FEXTRA
        if (pos + 2 > gz.size()) return fail("truncated");
        pos += 2 + (static_cast<uint8_t>(gz[pos]) | (static_cast<uint8_t>(gz[pos + 1]) << 8));
    }
    if (flags & 8) {   // FNAME
        while (pos < gz.size() && gz[pos] != '\0') ++pos;
        ++pos;
    }
    if (flags & 16) {   // FCOMMENT
        while (pos < gz.size() && gz[pos] != '\0') ++pos;
        ++pos;
    }
    if (flags & 2) pos += 2;   // FHCRC
    if (pos >= gz.size()) return fail("truncated");
    out->clear();
    Inflater inf(gz, out);
    inf.Seek(pos);
    if (const char* err = inf.Run()) return fail(err);
    const size_t end = inf.consumed();
    if (end + 8 > gz.size()) return fail("truncated trailer");
    auto u32 = [&](size_t p) {
        return static_cast<uint32_t>(static_cast<uint8_t>(gz[p])) | (static_cast<uint32_t>(static_cast<uint8_t>(gz[p + 1])) << 8)
               | (static_cast<uint32_t>(static_cast<uint8_t>(gz[p + 2])) << 16)
               | (static_cast<uint32_t>(static_cast<uint8_t>(gz[p + 3])) << 24);
    };
    if (u32(end) != Crc32(*out)) return fail("CRC mismatch");
    if (u32(end + 4) != static_cast<uint32_t>(out->size() & 0xFFFFFFFFu)) return fail("length mismatch");
    return true;
}

bool ExtractTar(const std::string& tar, const fs::path& dest, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = "tar: " + why;
        return false;
    };
    size_t pos = 0;
    std::string long_name;   // from a GNU 'L' entry or a pax 'path' record, for the next entry
    while (pos + 512 <= tar.size()) {
        const char* h = tar.data() + pos;
        bool zero = true;
        for (int i = 0; i < 512 && zero; ++i) zero = h[i] == '\0';
        if (zero) break;   // end of archive
        uint32_t sum = 0;
        for (int i = 0; i < 512; ++i) sum += (i >= 148 && i < 156) ? ' ' : static_cast<uint8_t>(h[i]);
        if (sum != Octal(h + 148, 8)) return fail("bad header checksum");
        const uint64_t size = Octal(h + 124, 12);
        const char type = h[156];
        std::string name = Field(h, 100);
        const std::string prefix = Field(h + 345, 155);
        if (!prefix.empty() && std::memcmp(h + 257, "ustar", 5) == 0) name = prefix + "/" + name;
        pos += 512;
        if (pos + size > tar.size()) return fail("truncated entry " + name);
        const std::string data = tar.substr(pos, static_cast<size_t>(size));
        pos += static_cast<size_t>((size + 511) / 512 * 512);

        if (type == 'L') {
            long_name = Field(data.data(), data.size());
            continue;
        }
        if (type == 'x') {
            // records: "<len> key=value\n"
            size_t p = 0;
            while (p < data.size()) {
                const size_t space = data.find(' ', p);
                if (space == std::string::npos) break;
                const size_t len = static_cast<size_t>(std::strtoul(data.c_str() + p, nullptr, 10));
                // the record is "<len> key=value\n": its space and its newline both lie inside it
                if (len == 0 || p + len > data.size() || space + 1 >= p + len || data[p + len - 1] != '\n') break;
                const std::string rec = data.substr(space + 1, p + len - space - 2);
                if (StartsWith(rec, "path=")) long_name = rec.substr(5);
                p += len;
            }
            continue;
        }
        if (type == 'g') continue;   // global pax header: nothing we need
        if (!long_name.empty()) {
            name = long_name;
            long_name.clear();
        }
        const std::string rel = SafeRelative(name);
        if (rel.empty()) {
            if (name == "." || name == "./" || name.empty()) continue;   // the archive's root itself
            return fail("refusing an entry outside the destination: " + name);
        }
        const fs::path target = dest / Path(rel);
        std::error_code ec;
        if (type == '5') {
            fs::create_directories(target, ec);
            if (ec) return fail("cannot create " + U8(target));
        } else if (type == '0' || type == '\0' || type == '7') {
            std::string err;
            if (!WriteFileAtomic(target, data, &err)) return fail(err);
        } else {
            return fail(std::string("refusing a link or special entry: ") + name);
        }
    }
    return true;
}

namespace {

uint32_t Le(const std::string& s, size_t p, int bytes) {
    uint32_t v = 0;
    for (int i = bytes - 1; i >= 0; --i) v = (v << 8) | static_cast<uint8_t>(s[p + static_cast<size_t>(i)]);
    return v;
}

}  // namespace

bool ListZip(const std::string& zip, std::vector<ZipEntry>* out, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = "zip: " + why;
        return false;
    };
    out->clear();
    // the end of central directory record: 22 bytes and a comment of up to 64 KB, at the end
    if (zip.size() < 22) return fail("too short");
    size_t eocd = std::string::npos;
    const size_t lowest = zip.size() > 22 + 0xFFFF ? zip.size() - 22 - 0xFFFF : 0;
    for (size_t p = zip.size() - 22 + 1; p-- > lowest;) {
        if (Le(zip, p, 4) == 0x06054b50u) {
            eocd = p;
            break;
        }
    }
    if (eocd == std::string::npos) return fail("no end of central directory (not a zip file)");
    const uint32_t count = Le(zip, eocd + 10, 2);
    const uint32_t cd_size = Le(zip, eocd + 12, 4);
    const uint32_t cd_offset = Le(zip, eocd + 16, 4);
    if (count == 0xFFFF || cd_offset == 0xFFFFFFFFu) return fail("zip64 archives are not supported");
    if (static_cast<uint64_t>(cd_offset) + cd_size > eocd) return fail("bad central directory");
    size_t p = cd_offset;
    for (uint32_t i = 0; i < count; ++i) {
        if (p + 46 > eocd || Le(zip, p, 4) != 0x02014b50u) return fail("bad central directory entry");
        ZipEntry e;
        const uint32_t flags = Le(zip, p + 8, 2);
        e.method = static_cast<uint16_t>(Le(zip, p + 10, 2));
        e.crc = Le(zip, p + 16, 4);
        e.compressed = Le(zip, p + 20, 4);
        e.size = Le(zip, p + 24, 4);
        const uint32_t name_len = Le(zip, p + 28, 2);
        const uint32_t extra_len = Le(zip, p + 30, 2);
        const uint32_t comment_len = Le(zip, p + 32, 2);
        e.local_offset = Le(zip, p + 42, 4);
        if (p + 46 + name_len + extra_len + comment_len > eocd) return fail("bad central directory entry");
        e.name = zip.substr(p + 46, name_len);
        for (char& c : e.name) {
            if (c == '\\') c = '/';
        }
        if (flags & 1) return fail("encrypted entry " + e.name);
        if (e.compressed == 0xFFFFFFFFu || e.size == 0xFFFFFFFFu || e.local_offset == 0xFFFFFFFFu) {
            return fail("zip64 entry " + e.name);
        }
        out->push_back(e);
        p += 46 + name_len + extra_len + comment_len;
    }
    return true;
}

bool ReadZipEntry(const std::string& zip, const ZipEntry& e, std::string* out, std::string* error) {
    auto fail = [&](const std::string& why) {
        if (error != nullptr) *error = "zip: " + e.name + ": " + why;
        return false;
    };
    const size_t p = static_cast<size_t>(e.local_offset);
    if (p + 30 > zip.size() || Le(zip, p, 4) != 0x04034b50u) return fail("bad local header");
    // the local header's own name and extra lengths (they may differ from the central directory's)
    const size_t data = p + 30 + Le(zip, p + 26, 2) + Le(zip, p + 28, 2);
    if (data + e.compressed > zip.size()) return fail("truncated");
    const std::string raw = zip.substr(data, static_cast<size_t>(e.compressed));
    if (e.method == 0) {
        *out = raw;
    } else if (e.method == 8) {
        std::string why;
        if (!Inflate(raw, out, &why)) return fail(why);
    } else {
        return fail("compression method " + std::to_string(e.method) + " is not supported");
    }
    if (out->size() != e.size) return fail("length mismatch");
    if (Crc32(*out) != e.crc) return fail("CRC mismatch");
    return true;
}

bool ExtractTarGz(const fs::path& archive, const fs::path& dest, std::string* error) {
    std::string gz, tar;
    if (!ReadFile(archive, &gz)) {
        if (error != nullptr) *error = "cannot read " + U8(archive);
        return false;
    }
    if (!Gunzip(gz, &tar, error)) return false;
    return ExtractTar(tar, dest, error);
}

}  // namespace atmt
