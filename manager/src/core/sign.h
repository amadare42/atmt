// sign.h - Ed25519 signatures over the release manifest, in minisign's file formats.
//
// docs/MANAGER.md, "Releases": manifest.json is checked against manifest.json.sig with the public
// key compiled into the app, and every download against its sha256 in the manifest. The formats are
// minisign's, so a release can be signed either with `atmt_manager --cli sign` or with minisign
// itself (`minisign -S -m manifest.json`), and checked with `minisign -V` by anyone:
//
//   public key   "untrusted comment: ...\n" + base64("Ed" | key id (8) | public key (32))
//   signature    "untrusted comment: ...\n" + base64(alg (2) | key id (8) | signature (64)) + "\n"
//                "trusted comment: ...\n"   + base64(global signature (64)) + "\n"
//
// alg "Ed" signs the file itself, "ED" (minisign's default) signs its BLAKE2b-512 hash; the global
// signature covers the signature and the trusted comment, so neither can be swapped. Both are
// verified. The secret key file is the manager's own (minisign encrypts its keys with scrypt and a
// password, which a release script cannot type): base64("Ed" | key id | seed (32) | public (32)).
#pragma once

#include <cstdint>
#include <string>

namespace atmt {

struct PublicKey {
    uint8_t key_id[8] = {};
    uint8_t key[32] = {};
};

// Parses a minisign public key: the file's text, or just its base64 line.
bool ParsePublicKey(const std::string& text, PublicKey* out, std::string* error = nullptr);

// True when `sig_file` (minisign .sig text) is a valid signature of `message` by `key`.
// `trusted_comment` receives the signed comment when given.
bool VerifySignature(const PublicKey& key, const std::string& message, const std::string& sig_file,
                     std::string* error = nullptr, std::string* trusted_comment = nullptr);

// A new key pair: the public key file's text and the secret key file's text.
bool GenerateKeyPair(std::string* public_file, std::string* secret_file, std::string* error = nullptr);

// A minisign "ED" signature file of `message` with the secret key file `secret_file`.
bool SignMessage(const std::string& secret_file, const std::string& message, const std::string& trusted_comment,
                 std::string* sig_file, std::string* error = nullptr);

std::string Base64Encode(const std::string& data);
bool Base64Decode(const std::string& text, std::string* out);

}  // namespace atmt
