// hash.h - MD5 (the install manifest, the ed8.exe check) and SHA-256 (release assets).
#pragma once

#include <cstdint>
#include <string>

#include "util.h"

namespace atmt {

class Md5 {
public:
    Md5();
    void Update(const void* data, size_t size);
    std::string HexDigest();   // finalises

private:
    void Block(const uint8_t* p);
    uint32_t a_, b_, c_, d_;
    uint64_t length_ = 0;
    uint8_t buffer_[64];
    size_t used_ = 0;
};

class Sha256 {
public:
    Sha256();
    void Update(const void* data, size_t size);
    std::string HexDigest();   // finalises

private:
    void Block(const uint8_t* p);
    uint32_t h_[8];
    uint64_t length_ = 0;
    uint8_t buffer_[64];
    size_t used_ = 0;
};

std::string Md5Hex(const std::string& data);
std::string Sha256Hex(const std::string& data);
// Of a file's contents; empty when it cannot be read.
std::string Md5File(const fs::path& p);
std::string Sha256File(const fs::path& p);

std::string ToHex(const uint8_t* data, size_t size);
bool FromHex(const std::string& hex, uint8_t* out, size_t size);

}  // namespace atmt
