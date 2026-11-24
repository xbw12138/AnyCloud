//
// cloud: small self-contained crypto helpers used by the drive providers.
//
// 115's web download endpoint obfuscates its request/response payloads with
// RSA + XOR + base64 ("m115"), and Baidu signs its download requests with
// MD5/SHA1 + RC4. The console has no OpenSSL, so everything lives here.
//

#pragma once

#include <array>
#include <cstdint>
#include <string>

namespace cloud::crypto {

std::string base64Encode(const std::string& data);
std::string base64Decode(const std::string& data);

std::string md5Hex(const std::string& data);
std::string sha1Hex(const std::string& data);

/// Streaming hash over a local file (used by the upload APIs, which need the
/// MD5 of each 4 MB slice / the SHA1 of the whole file without loading it).
/// Returns an empty string when the file cannot be read.
std::string md5FileHex(const std::string& path, uint64_t offset, uint64_t size);
std::string sha1FileHex(const std::string& path);

/// RC4 stream cipher (Baidu's download sign).
std::string rc4(const std::string& key, const std::string& data);

using M115Key = std::array<unsigned char, 16>;

M115Key m115GenerateKey();
std::string m115Encode(const std::string& plain, const M115Key& key);
std::string m115Decode(const std::string& encoded, const M115Key& key);

}  // namespace cloud::crypto
