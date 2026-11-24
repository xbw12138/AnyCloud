//
// cloud: small self-contained crypto helpers used by the drive providers.
//

#include "cloud/util/crypto.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <sys/types.h>
#include <vector>

namespace cloud::crypto {

namespace {

const char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int base64Value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

inline uint32_t rotl32(uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

// -----------------------------------------------------------------------------
// MD5
// -----------------------------------------------------------------------------

struct Md5Context {
    uint32_t state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    uint64_t bits = 0;
    unsigned char buffer[64] = {};
    size_t buffered = 0;
};

void md5Transform(Md5Context& ctx, const unsigned char* block) {
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a,
        0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be,
        0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340,
        0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
        0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c,
        0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa,
        0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92,
        0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
        0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
    static const int S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7,
                              12, 17, 22, 5, 9,  14, 20, 5, 9,  14, 20, 5,  9,
                              14, 20, 5, 9,  14, 20, 4, 11, 16, 23, 4, 11, 16,
                              23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21,
                              6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};

    uint32_t M[16];
    for (int i = 0; i < 16; i++) {
        M[i] = (uint32_t)block[i * 4] | ((uint32_t)block[i * 4 + 1] << 8) |
               ((uint32_t)block[i * 4 + 2] << 16) |
               ((uint32_t)block[i * 4 + 3] << 24);
    }

    uint32_t a = ctx.state[0], b = ctx.state[1], c = ctx.state[2],
             d = ctx.state[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        uint32_t tmp = d;
        d = c;
        c = b;
        b = b + rotl32(a + f + K[i] + M[g], S[i]);
        a = tmp;
    }
    ctx.state[0] += a;
    ctx.state[1] += b;
    ctx.state[2] += c;
    ctx.state[3] += d;
}

void md5Update(Md5Context& ctx, const unsigned char* data, size_t size) {
    ctx.bits += (uint64_t)size * 8;
    while (size > 0) {
        size_t take = std::min(size, 64 - ctx.buffered);
        std::memcpy(ctx.buffer + ctx.buffered, data, take);
        ctx.buffered += take;
        data += take;
        size -= take;
        if (ctx.buffered == 64) {
            md5Transform(ctx, ctx.buffer);
            ctx.buffered = 0;
        }
    }
}

std::string md5Finish(Md5Context& ctx) {
    // The message length must be the one collected so far: the padding below
    // runs through md5Update and would otherwise end up in the length field.
    uint64_t bits = ctx.bits;
    unsigned char padding[64] = {0x80};
    size_t padLen = ctx.buffered < 56 ? 56 - ctx.buffered : 120 - ctx.buffered;
    md5Update(ctx, padding, padLen);
    unsigned char length[8];
    for (int i = 0; i < 8; i++) length[i] = (unsigned char)(bits >> (8 * i));
    md5Update(ctx, length, 8);

    unsigned char digest[16];
    for (int i = 0; i < 4; i++) {
        digest[i * 4] = (unsigned char)ctx.state[i];
        digest[i * 4 + 1] = (unsigned char)(ctx.state[i] >> 8);
        digest[i * 4 + 2] = (unsigned char)(ctx.state[i] >> 16);
        digest[i * 4 + 3] = (unsigned char)(ctx.state[i] >> 24);
    }
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (unsigned char byte : digest) {
        out += hex[byte >> 4];
        out += hex[byte & 0xf];
    }
    return out;
}

std::string md5HexImpl(const std::string& data) {
    Md5Context ctx;
    md5Update(ctx, (const unsigned char*)data.data(), data.size());
    return md5Finish(ctx);
}

// -----------------------------------------------------------------------------
// SHA-1
// -----------------------------------------------------------------------------

struct Sha1Context {
    uint32_t state[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476,
                         0xc3d2e1f0};
    uint64_t bits = 0;
    unsigned char buffer[64] = {};
    size_t buffered = 0;
};

void sha1Transform(Sha1Context& ctx, const unsigned char* block) {
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               (uint32_t)block[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++)
        w[i] = rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = ctx.state[0], b = ctx.state[1], c = ctx.state[2],
             d = ctx.state[3], e = ctx.state[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        uint32_t temp = rotl32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl32(b, 30);
        b = a;
        a = temp;
    }
    ctx.state[0] += a;
    ctx.state[1] += b;
    ctx.state[2] += c;
    ctx.state[3] += d;
    ctx.state[4] += e;
}

void sha1Update(Sha1Context& ctx, const unsigned char* data, size_t size) {
    ctx.bits += (uint64_t)size * 8;
    while (size > 0) {
        size_t take = std::min(size, 64 - ctx.buffered);
        std::memcpy(ctx.buffer + ctx.buffered, data, take);
        ctx.buffered += take;
        data += take;
        size -= take;
        if (ctx.buffered == 64) {
            sha1Transform(ctx, ctx.buffer);
            ctx.buffered = 0;
        }
    }
}

std::string sha1Finish(Sha1Context& ctx) {
    // Same as md5Finish: capture the real length before adding the padding.
    uint64_t bits = ctx.bits;
    unsigned char padding[64] = {0x80};
    size_t padLen = ctx.buffered < 56 ? 56 - ctx.buffered : 120 - ctx.buffered;
    sha1Update(ctx, padding, padLen);
    unsigned char length[8];
    for (int i = 0; i < 8; i++) length[i] = (unsigned char)(bits >> (56 - 8 * i));
    sha1Update(ctx, length, 8);

    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(40);
    for (int i = 0; i < 5; i++) {
        for (int shift = 24; shift >= 0; shift -= 8) {
            unsigned char byte = (unsigned char)(ctx.state[i] >> shift);
            out += hex[byte >> 4];
            out += hex[byte & 0xf];
        }
    }
    return out;
}

std::string sha1HexImpl(const std::string& data) {
    Sha1Context ctx;
    sha1Update(ctx, (const unsigned char*)data.data(), data.size());
    return sha1Finish(ctx);
}

// -----------------------------------------------------------------------------
// 115 "m115" payload (RSA + XOR + base64)
// -----------------------------------------------------------------------------

const unsigned char kXorKeySeed[] = {
    0xf0, 0xe5, 0x69, 0xae, 0xbf, 0xdc, 0xbf, 0x8a, 0x1a, 0x45, 0xe8, 0xbe,
    0x7d, 0xa6, 0x73, 0xb8, 0xde, 0x8f, 0xe7, 0xc4, 0x45, 0xda, 0x86, 0xc4,
    0x9b, 0x64, 0x8b, 0x14, 0x6a, 0xb4, 0xf1, 0xaa, 0x38, 0x01, 0x35, 0x9e,
    0x26, 0x69, 0x2c, 0x86, 0x00, 0x6b, 0x4f, 0xa5, 0x36, 0x34, 0x62, 0xa6,
    0x2a, 0x96, 0x68, 0x18, 0xf2, 0x4a, 0xfd, 0xbd, 0x6b, 0x97, 0x8f, 0x4d,
    0x8f, 0x89, 0x13, 0xb7, 0x6c, 0x8e, 0x93, 0xed, 0x0e, 0x0d, 0x48, 0x3e,
    0xd7, 0x2f, 0x88, 0xd8, 0xfe, 0xfe, 0x7e, 0x86, 0x50, 0x95, 0x4f, 0xd1,
    0xeb, 0x83, 0x26, 0x34, 0xdb, 0x66, 0x7b, 0x9c, 0x7e, 0x9d, 0x7a, 0x81,
    0x32, 0xea, 0xb6, 0x33, 0xde, 0x3a, 0xa9, 0x59, 0x34, 0x66, 0x3b, 0xaa,
    0xba, 0x81, 0x60, 0x48, 0xb9, 0xd5, 0x81, 0x9c, 0xf8, 0x6c, 0x84, 0x77,
    0xff, 0x54, 0x78, 0x26, 0x5f, 0xbe, 0xe8, 0x1e, 0x36, 0x9f, 0x34, 0x80,
    0x5c, 0x45, 0x2c, 0x9b, 0x76, 0xd5, 0x1b, 0x8f, 0xcc, 0xc3, 0xb8, 0xf5,
};

const unsigned char kXorClientKey[] = {0x78, 0x06, 0xad, 0x4c, 0x33,
                                       0x86, 0x5d, 0x18, 0x4c, 0x01,
                                       0x3f, 0x46};

const char* kRsaModulusHex =
    "8686980c0f5a24c4b9d43020cd2c22703ff3f450756529058b1cf88f09b86021"
    "36477198a6e2683149659bd122c33592fdb5ad47944ad1ea4d36c6b172aad633"
    "8c3bb6ac6227502d010993ac967d1aef00f0c8e038de2e4d3bc2ec368af2e9f1"
    "0a6f1eda4f7262f136420c07c331b871bf139f74f3010e3c4fe57df3afb71683";

const int kRsaKeyBytes = 128;

std::vector<unsigned char> xorDeriveKey(const unsigned char* seed, int size) {
    std::vector<unsigned char> key(size);
    for (int i = 0; i < size; i++) {
        key[i] = (unsigned char)((seed[i] + kXorKeySeed[size * i]) & 0xff);
        key[i] ^= kXorKeySeed[size * (size - i - 1)];
    }
    return key;
}

void xorTransform(unsigned char* data, size_t size,
                  const std::vector<unsigned char>& key) {
    if (key.empty()) return;
    size_t mod = size % 4;
    for (size_t i = 0; i < mod; i++) data[i] ^= key[i % key.size()];
    for (size_t i = mod; i < size; i++)
        data[i] ^= key[(i - mod) % key.size()];
}

class BigUInt {
public:
    std::vector<uint32_t> w;  // little endian limbs

    void trim() {
        while (!this->w.empty() && this->w.back() == 0) this->w.pop_back();
    }

    bool isZero() const { return this->w.empty(); }

    int bitLength() const {
        if (this->w.empty()) return 0;
        uint32_t top = this->w.back();
        int bits = 0;
        while (top) {
            bits++;
            top >>= 1;
        }
        return (int)(this->w.size() - 1) * 32 + bits;
    }

    bool bit(size_t index) const {
        size_t limb = index / 32;
        if (limb >= this->w.size()) return false;
        return ((this->w[limb] >> (index % 32)) & 1u) != 0;
    }

    void shiftLeft1() {
        uint32_t carry = 0;
        for (auto& limb : this->w) {
            uint32_t next = limb >> 31;
            limb = (limb << 1) | carry;
            carry = next;
        }
        if (carry) this->w.push_back(carry);
    }

    void orBit0() {
        if (this->w.empty()) this->w.push_back(0);
        this->w[0] |= 1u;
    }

    static int compare(const BigUInt& a, const BigUInt& b) {
        if (a.w.size() != b.w.size())
            return a.w.size() < b.w.size() ? -1 : 1;
        for (size_t i = a.w.size(); i-- > 0;) {
            if (a.w[i] != b.w[i]) return a.w[i] < b.w[i] ? -1 : 1;
        }
        return 0;
    }

    void subtract(const BigUInt& other) {
        int64_t borrow = 0;
        for (size_t i = 0; i < this->w.size(); i++) {
            int64_t current = (int64_t)this->w[i] - borrow -
                              (i < other.w.size() ? (int64_t)other.w[i] : 0);
            if (current < 0) {
                current += (int64_t)((uint64_t)1 << 32);
                borrow = 1;
            } else {
                borrow = 0;
            }
            this->w[i] = (uint32_t)current;
        }
        this->trim();
    }

    static BigUInt fromBytes(const unsigned char* data, size_t size) {
        BigUInt value;
        value.w.assign((size + 3) / 4, 0);
        for (size_t i = 0; i < size; i++) {
            size_t pos = size - 1 - i;
            value.w[pos / 4] |= (uint32_t)data[i] << (8 * (pos % 4));
        }
        value.trim();
        return value;
    }

    static BigUInt fromHex(const std::string& hex) {
        std::vector<unsigned char> bytes;
        bytes.reserve(hex.size() / 2);
        for (size_t i = 0; i + 1 < hex.size(); i += 2) {
            auto value = [](char c) -> unsigned {
                if (c >= '0' && c <= '9') return (unsigned)(c - '0');
                if (c >= 'a' && c <= 'f') return (unsigned)(c - 'a' + 10);
                if (c >= 'A' && c <= 'F') return (unsigned)(c - 'A' + 10);
                return 0;
            };
            bytes.push_back((unsigned char)((value(hex[i]) << 4) |
                                            value(hex[i + 1])));
        }
        return fromBytes(bytes.data(), bytes.size());
    }

    std::vector<unsigned char> toBytes(size_t size) const {
        std::vector<unsigned char> out(size, 0);
        for (size_t i = 0; i < size; i++) {
            size_t pos = size - 1 - i;
            size_t limb = pos / 4;
            unsigned shift = 8 * (unsigned)(pos % 4);
            out[i] = limb < this->w.size()
                         ? (unsigned char)((this->w[limb] >> shift) & 0xff)
                         : 0;
        }
        return out;
    }
};

BigUInt multiply(const BigUInt& a, const BigUInt& b) {
    BigUInt result;
    if (a.w.empty() || b.w.empty()) return result;
    result.w.assign(a.w.size() + b.w.size() + 1, 0);
    for (size_t i = 0; i < a.w.size(); i++) {
        uint64_t carry = 0;
        for (size_t j = 0; j < b.w.size(); j++) {
            uint64_t current = result.w[i + j] +
                               (uint64_t)a.w[i] * (uint64_t)b.w[j] + carry;
            result.w[i + j] = (uint32_t)current;
            carry = current >> 32;
        }
        size_t k = i + b.w.size();
        while (carry) {
            uint64_t current = result.w[k] + carry;
            result.w[k] = (uint32_t)current;
            carry = current >> 32;
            k++;
        }
    }
    result.trim();
    return result;
}

BigUInt modulo(const BigUInt& value, const BigUInt& modulus) {
    BigUInt result;
    if (modulus.isZero()) return result;
    for (int bit = value.bitLength() - 1; bit >= 0; bit--) {
        result.shiftLeft1();
        if (value.bit((size_t)bit)) result.orBit0();
        if (BigUInt::compare(result, modulus) >= 0) result.subtract(modulus);
    }
    return result;
}

BigUInt modPow(const BigUInt& base, uint32_t exponent,
               const BigUInt& modulus) {
    BigUInt result = BigUInt::fromBytes((const unsigned char*)"\x01", 1);
    BigUInt current = modulo(base, modulus);
    int highest = 0;
    for (int bit = 31; bit >= 0; bit--) {
        if (exponent & (1u << bit)) {
            highest = bit;
            break;
        }
    }
    for (int bit = highest; bit >= 0; bit--) {
        result = modulo(multiply(result, result), modulus);
        if (exponent & (1u << bit))
            result = modulo(multiply(result, current), modulus);
    }
    return result;
}

std::vector<unsigned char> rsaCipher(const unsigned char* data, size_t size) {
    static const BigUInt modulus = BigUInt::fromHex(kRsaModulusHex);
    BigUInt message = BigUInt::fromBytes(data, size);
    BigUInt encrypted = modPow(message, 0x10001, modulus);
    return encrypted.toBytes(kRsaKeyBytes);
}

std::vector<unsigned char> rsaEncrypt(const std::string& input) {
    std::random_device device;
    std::mt19937 rng(device());
    std::uniform_int_distribution<int> byte(0, 255);

    std::string output;
    const size_t slice = kRsaKeyBytes - 11;
    size_t offset = 0;
    while (offset < input.size()) {
        size_t take = std::min(slice, input.size() - offset);
        std::vector<unsigned char> block(kRsaKeyBytes, 0);
        block[0] = 0;
        block[1] = 2;
        size_t padSize = kRsaKeyBytes - take - 3;
        for (size_t i = 0; i < padSize; i++)
            block[2 + i] = (unsigned char)((byte(rng) % 0xff) + 1);
        block[padSize + 2] = 0;
        std::memcpy(block.data() + padSize + 3, input.data() + offset, take);
        auto encrypted = rsaCipher(block.data(), block.size());
        output.append((const char*)encrypted.data(), encrypted.size());
        offset += take;
    }
    return std::vector<unsigned char>(output.begin(), output.end());
}

std::string rsaDecrypt(const std::vector<unsigned char>& input) {
    std::string output;
    for (size_t offset = 0; offset < input.size(); offset += kRsaKeyBytes) {
        size_t take = std::min((size_t)kRsaKeyBytes, input.size() - offset);
        auto plain = rsaCipher(input.data() + offset, take);
        // Strip PKCS#1 padding: 00 02 <random> 00 <data>.
        for (size_t i = 1; i < plain.size(); i++) {
            if (plain[i] == 0) {
                output.append((const char*)plain.data() + i + 1,
                              plain.size() - i - 1);
                break;
            }
        }
    }
    return output;
}

}  // namespace

std::string base64Encode(const std::string& data) {
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    size_t i = 0;
    while (i + 2 < data.size()) {
        uint32_t value = ((uint32_t)(unsigned char)data[i] << 16) |
                         ((uint32_t)(unsigned char)data[i + 1] << 8) |
                         (uint32_t)(unsigned char)data[i + 2];
        out += kBase64Chars[(value >> 18) & 63];
        out += kBase64Chars[(value >> 12) & 63];
        out += kBase64Chars[(value >> 6) & 63];
        out += kBase64Chars[value & 63];
        i += 3;
    }
    size_t remain = data.size() - i;
    if (remain == 1) {
        uint32_t value = (uint32_t)(unsigned char)data[i] << 16;
        out += kBase64Chars[(value >> 18) & 63];
        out += kBase64Chars[(value >> 12) & 63];
        out += "==";
    } else if (remain == 2) {
        uint32_t value = ((uint32_t)(unsigned char)data[i] << 16) |
                         ((uint32_t)(unsigned char)data[i + 1] << 8);
        out += kBase64Chars[(value >> 18) & 63];
        out += kBase64Chars[(value >> 12) & 63];
        out += kBase64Chars[(value >> 6) & 63];
        out += '=';
    }
    return out;
}

std::string base64Decode(const std::string& data) {
    std::string out;
    out.reserve(data.size() * 3 / 4);
    uint32_t value = 0;
    int bits = 0;
    for (char c : data) {
        if (c == '=') break;
        int v = base64Value((unsigned char)c);
        if (v < 0) continue;
        value = (value << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char)((value >> bits) & 0xff);
        }
    }
    return out;
}

std::string md5Hex(const std::string& data) { return md5HexImpl(data); }

std::string sha1Hex(const std::string& data) { return sha1HexImpl(data); }

namespace {

/// Feeds `size` bytes starting at `offset` of `path` into `update`. Returns
/// false when the file cannot be opened or read.
template <typename UpdateFn>
bool hashFile(const std::string& path, uint64_t offset, uint64_t size,
              UpdateFn update) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) return false;
    if (offset > 0 && fseeko(file, (off_t)offset, SEEK_SET) != 0) {
        fclose(file);
        return false;
    }
    std::vector<unsigned char> buffer(1 << 20);
    uint64_t remaining = size;
    bool ok = true;
    while (remaining > 0) {
        size_t want = (size_t)std::min<uint64_t>(remaining, buffer.size());
        size_t got = fread(buffer.data(), 1, want, file);
        if (got == 0) {
            ok = false;
            break;
        }
        update(buffer.data(), got);
        remaining -= got;
    }
    fclose(file);
    return ok;
}

}  // namespace

std::string md5FileHex(const std::string& path, uint64_t offset, uint64_t size) {
    Md5Context ctx;
    if (!hashFile(path, offset, size, [&ctx](const unsigned char* data,
                                             size_t length) {
            md5Update(ctx, data, length);
        }))
        return std::string();
    return md5Finish(ctx);
}

std::string sha1FileHex(const std::string& path) {
    FILE* probe = fopen(path.c_str(), "rb");
    if (!probe) return std::string();
    fseeko(probe, 0, SEEK_END);
    off_t size = ftello(probe);
    fclose(probe);
    if (size < 0) return std::string();
    Sha1Context ctx;
    if (!hashFile(path, 0, (uint64_t)size, [&ctx](const unsigned char* data,
                                                  size_t length) {
            sha1Update(ctx, data, length);
        }))
        return std::string();
    return sha1Finish(ctx);
}

std::string rc4(const std::string& key, const std::string& data) {
    unsigned char s[256];
    for (int i = 0; i < 256; i++) s[i] = (unsigned char)i;
    int j = 0;
    for (int i = 0; i < 256; i++) {
        j = (j + s[i] + (unsigned char)key[i % key.size()]) & 0xff;
        std::swap(s[i], s[j]);
    }
    std::string out = data;
    int i = 0;
    j = 0;
    for (char& c : out) {
        i = (i + 1) & 0xff;
        j = (j + s[i]) & 0xff;
        std::swap(s[i], s[j]);
        c ^= (char)s[(s[i] + s[j]) & 0xff];
    }
    return out;
}

M115Key m115GenerateKey() {
    M115Key key{};
    std::random_device device;
    std::mt19937 rng(device());
    for (auto& byte : key) byte = (unsigned char)(rng() & 0xff);
    return key;
}

std::string m115Encode(const std::string& plain, const M115Key& key) {
    std::string buffer;
    buffer.append((const char*)key.data(), key.size());
    buffer += plain;

    unsigned char* payload = (unsigned char*)buffer.data() + key.size();
    size_t payloadSize = buffer.size() - key.size();
    auto derived = xorDeriveKey(key.data(), 4);
    xorTransform(payload, payloadSize, derived);
    std::reverse(payload, payload + payloadSize);
    std::vector<unsigned char> clientKey(kXorClientKey,
                                         kXorClientKey + sizeof(kXorClientKey));
    xorTransform(payload, payloadSize, clientKey);

    auto encrypted = rsaEncrypt(buffer);
    return base64Encode(
        std::string((const char*)encrypted.data(), encrypted.size()));
}

std::string m115Decode(const std::string& encoded, const M115Key& key) {
    std::vector<unsigned char> raw = [&] {
        std::string bytes = base64Decode(encoded);
        return std::vector<unsigned char>(bytes.begin(), bytes.end());
    }();
    std::string data = rsaDecrypt(raw);
    if (data.size() <= 16) return std::string();

    std::string output = data.substr(16);
    auto prefix = xorDeriveKey((const unsigned char*)data.data(), 12);
    xorTransform((unsigned char*)output.data(), output.size(), prefix);
    std::reverse(output.begin(), output.end());
    auto derived = xorDeriveKey(key.data(), 4);
    xorTransform((unsigned char*)output.data(), output.size(), derived);
    return output;
}

}  // namespace cloud::crypto
