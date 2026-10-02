// sign.cpp - see sign.h. The arithmetic is Monocypher's (third_party/monocypher).
#include "sign.h"

#include <cstring>
#include <vector>

#include "hash.h"
#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "util.h"

#ifdef _WIN32
#include <windows.h>
#include <bcrypt.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace atmt {

namespace {

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

bool Random(uint8_t* out, size_t size) {
#ifdef _WIN32
    return BCryptGenRandom(nullptr, out, static_cast<ULONG>(size), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#else
    const int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return false;
    size_t got = 0;
    while (got < size) {
        const ssize_t n = read(fd, out + got, size - got);
        if (n <= 0) break;
        got += static_cast<size_t>(n);
    }
    close(fd);
    return got == size;
#endif
}

// The base64 payload line of a minisign file: the first line that is not a comment.
std::vector<std::string> PayloadLines(const std::string& text) {
    std::vector<std::string> out;
    for (const std::string& line : Lines(text)) {
        const std::string t = Trim(line);
        if (t.empty() || StartsWith(t, "untrusted comment:") || StartsWith(t, "trusted comment:")) continue;
        out.push_back(t);
    }
    return out;
}

bool Fail(std::string* error, const std::string& why) {
    if (error != nullptr) *error = why;
    return false;
}

}  // namespace

std::string Base64Encode(const std::string& data) {
    std::string out;
    size_t i = 0;
    while (i + 2 < data.size()) {
        const uint32_t v = (static_cast<uint8_t>(data[i]) << 16) | (static_cast<uint8_t>(data[i + 1]) << 8)
                           | static_cast<uint8_t>(data[i + 2]);
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += kB64[v & 63];
        i += 3;
    }
    if (i + 1 == data.size()) {
        const uint32_t v = static_cast<uint8_t>(data[i]) << 16;
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == data.size()) {
        const uint32_t v = (static_cast<uint8_t>(data[i]) << 16) | (static_cast<uint8_t>(data[i + 1]) << 8);
        out += kB64[(v >> 18) & 63];
        out += kB64[(v >> 12) & 63];
        out += kB64[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

bool Base64Decode(const std::string& text, std::string* out) {
    out->clear();
    uint32_t v = 0;
    int bits = 0;
    size_t pad = 0;
    for (char c : text) {
        if (c == '=') {
            ++pad;
            continue;
        }
        if (pad != 0) return false;   // data after padding
        const char* p = std::strchr(kB64, c);
        if (c == '\0' || p == nullptr) return false;
        v = (v << 6) | static_cast<uint32_t>(p - kB64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            *out += static_cast<char>((v >> bits) & 0xFF);
        }
    }
    return pad <= 2;
}

bool ParsePublicKey(const std::string& text, PublicKey* out, std::string* error) {
    const std::vector<std::string> lines = PayloadLines(text);
    if (lines.empty()) return Fail(error, "no public key in the text");
    std::string raw;
    if (!Base64Decode(lines[0], &raw) || raw.size() != 42) return Fail(error, "not a minisign public key");
    if (raw[0] != 'E' || raw[1] != 'd') return Fail(error, "unsupported public key algorithm");
    std::memcpy(out->key_id, raw.data() + 2, 8);
    std::memcpy(out->key, raw.data() + 10, 32);
    return true;
}

bool VerifySignature(const PublicKey& key, const std::string& message, const std::string& sig_file,
                     std::string* error, std::string* trusted_comment) {
    std::string comment;
    std::vector<std::string> payload;
    for (const std::string& line : Lines(sig_file)) {
        const std::string t = Trim(line);
        if (StartsWith(t, "trusted comment: ")) {
            comment = t.substr(17);
        } else if (!t.empty() && !StartsWith(t, "untrusted comment:") && !StartsWith(t, "trusted comment:")) {
            payload.push_back(t);
        }
    }
    if (payload.size() != 2) return Fail(error, "malformed signature file");
    std::string sig, global;
    if (!Base64Decode(payload[0], &sig) || sig.size() != 74) return Fail(error, "malformed signature");
    if (!Base64Decode(payload[1], &global) || global.size() != 64) return Fail(error, "malformed global signature");
    if (std::memcmp(sig.data() + 2, key.key_id, 8) != 0) {
        return Fail(error, "signed with another key (key id " + ToHex(reinterpret_cast<const uint8_t*>(sig.data() + 2), 8)
                               + ", expected " + ToHex(key.key_id, 8) + ")");
    }
    const uint8_t* s = reinterpret_cast<const uint8_t*>(sig.data() + 10);
    int bad;
    if (sig[0] == 'E' && sig[1] == 'd') {
        bad = crypto_ed25519_check(s, key.key, reinterpret_cast<const uint8_t*>(message.data()), message.size());
    } else if (sig[0] == 'E' && sig[1] == 'D') {
        uint8_t hash[64];
        crypto_blake2b(hash, sizeof(hash), reinterpret_cast<const uint8_t*>(message.data()), message.size());
        bad = crypto_ed25519_check(s, key.key, hash, sizeof(hash));
    } else {
        return Fail(error, "unsupported signature algorithm");
    }
    if (bad != 0) return Fail(error, "signature does not match");
    const std::string signed_comment = std::string(sig.data() + 10, 64) + comment;
    if (crypto_ed25519_check(reinterpret_cast<const uint8_t*>(global.data()), key.key,
                             reinterpret_cast<const uint8_t*>(signed_comment.data()), signed_comment.size())
        != 0) {
        return Fail(error, "trusted comment signature does not match");
    }
    if (trusted_comment != nullptr) *trusted_comment = comment;
    return true;
}

bool GenerateKeyPair(std::string* public_file, std::string* secret_file, std::string* error) {
    uint8_t seed[32], key_id[8], secret[64], pub[32];
    if (!Random(seed, sizeof(seed)) || !Random(key_id, sizeof(key_id))) return Fail(error, "no random numbers");
    uint8_t seed_copy[32];
    std::memcpy(seed_copy, seed, 32);
    crypto_ed25519_key_pair(secret, pub, seed_copy);   // wipes seed_copy
    const std::string id(reinterpret_cast<char*>(key_id), 8);
    const std::string id_hex = ToHex(key_id, 8);
    *public_file = "untrusted comment: atmt release public key " + id_hex + "\n"
                   + Base64Encode("Ed" + id + std::string(reinterpret_cast<char*>(pub), 32)) + "\n";
    *secret_file = "untrusted comment: atmt release SECRET key " + id_hex + " - never commit or upload this\n"
                   + Base64Encode("Ed" + id + std::string(reinterpret_cast<char*>(seed), 32)
                                  + std::string(reinterpret_cast<char*>(pub), 32))
                   + "\n";
    crypto_wipe(seed, sizeof(seed));
    crypto_wipe(secret, sizeof(secret));
    return true;
}

bool SignMessage(const std::string& secret_file, const std::string& message, const std::string& trusted_comment,
                 std::string* sig_file, std::string* error) {
    const std::vector<std::string> lines = PayloadLines(secret_file);
    std::string raw;
    if (lines.empty() || !Base64Decode(lines[0], &raw) || raw.size() != 74 || raw[0] != 'E' || raw[1] != 'd') {
        return Fail(error, "not an atmt secret key file");
    }
    uint8_t seed[32], secret[64], pub[32];
    std::memcpy(seed, raw.data() + 10, 32);
    crypto_ed25519_key_pair(secret, pub, seed);
    if (std::memcmp(pub, raw.data() + 42, 32) != 0) {
        crypto_wipe(secret, sizeof(secret));
        return Fail(error, "the secret key file is damaged (its public half does not match)");
    }
    uint8_t hash[64];
    crypto_blake2b(hash, sizeof(hash), reinterpret_cast<const uint8_t*>(message.data()), message.size());
    uint8_t sig[64];
    crypto_ed25519_sign(sig, secret, hash, sizeof(hash));
    const std::string sig_str(reinterpret_cast<char*>(sig), 64);
    const std::string signed_comment = sig_str + trusted_comment;
    uint8_t global[64];
    crypto_ed25519_sign(global, secret, reinterpret_cast<const uint8_t*>(signed_comment.data()), signed_comment.size());
    crypto_wipe(secret, sizeof(secret));
    *sig_file = "untrusted comment: signature from atmt_manager\n" + Base64Encode("ED" + raw.substr(2, 8) + sig_str)
                + "\ntrusted comment: " + trusted_comment + "\n"
                + Base64Encode(std::string(reinterpret_cast<char*>(global), 64)) + "\n";
    return true;
}

}  // namespace atmt
