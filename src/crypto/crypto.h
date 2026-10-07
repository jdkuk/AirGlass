// Crypto primitives used by the AirPlay protocol.
//  - SHA-512, Ed25519 (orlp/ed25519), X25519 (ref10 ladder on orlp field arithmetic)
//  - AES-128 CTR (continuous keystream) and CBC via Windows CNG
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace crypto {

void RandomBytes(void* out, size_t n);

class Sha512 {
public:
    Sha512();
    void Update(const void* data, size_t n);
    void Final(uint8_t out[64]);
private:
    alignas(8) uint8_t ctx_[256];
};

void Sha512Digest(const void* data, size_t n, uint8_t out[64]);

// X25519 (RFC 7748).
void X25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]);
void X25519PublicKey(uint8_t pub[32], const uint8_t priv[32]);

// Ed25519 (RFC 8032) with orlp key format: priv is the 64-byte expanded secret.
void Ed25519KeypairFromSeed(const uint8_t seed[32], uint8_t pub[32], uint8_t priv[64]);
void Ed25519Sign(uint8_t sig[64], const uint8_t* msg, size_t n, const uint8_t pub[32], const uint8_t priv[64]);
bool Ed25519Verify(const uint8_t sig[64], const uint8_t* msg, size_t n, const uint8_t pub[32]);

// AES-128-CTR with a continuous keystream across calls (128-bit big-endian counter).
class AesCtr {
public:
    AesCtr() = default;
    ~AesCtr();
    AesCtr(const AesCtr&) = delete;
    AesCtr& operator=(const AesCtr&) = delete;
    bool Init(const uint8_t key[16], const uint8_t iv[16]);
    void Process(uint8_t* data, size_t n);
    bool Valid() const { return key_ != nullptr; }
private:
    void Refill();
    void* key_ = nullptr;  // BCRYPT_KEY_HANDLE
    uint8_t ctr_[16]{};
    std::vector<uint8_t> in_, ks_;
    size_t pos_ = 0;
};

// AES-128-CBC, no padding. Each call starts from the given IV.
class AesCbc {
public:
    AesCbc() = default;
    ~AesCbc();
    AesCbc(const AesCbc&) = delete;
    AesCbc& operator=(const AesCbc&) = delete;
    bool Init(const uint8_t key[16]);
    bool Decrypt(const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t n);
    bool Encrypt(const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t n);
private:
    void* key_ = nullptr;
};

// Known-answer tests for all of the above; returns false and logs on mismatch.
bool SelfTest();

}  // namespace crypto
