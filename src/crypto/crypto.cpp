#include "crypto/crypto.h"

#include "common.h"

#include <bcrypt.h>

extern "C" {
#include "ed25519.h"
#include "fe.h"
#include "sha512.h"
}

namespace crypto {

static_assert(sizeof(sha512_context) <= 256, "sha512 context too large");

void RandomBytes(void* out, size_t n) {
    BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
}

Sha512::Sha512() { sha512_init(reinterpret_cast<sha512_context*>(ctx_)); }

void Sha512::Update(const void* data, size_t n) {
    sha512_update(reinterpret_cast<sha512_context*>(ctx_), static_cast<const unsigned char*>(data), n);
}

void Sha512::Final(uint8_t out[64]) { sha512_final(reinterpret_cast<sha512_context*>(ctx_), out); }

void Sha512Digest(const void* data, size_t n, uint8_t out[64]) {
    sha512(static_cast<const unsigned char*>(data), n, out);
}

void X25519(uint8_t out[32], const uint8_t scalar[32], const uint8_t point[32]) {
    uint8_t e[32];
    std::memcpy(e, scalar, 32);
    e[0] &= 248;
    e[31] &= 127;
    e[31] |= 64;

    fe x1, x2, z2, x3, z3, tmp0, tmp1;
    fe_frombytes(x1, point);
    fe_1(x2);
    fe_0(z2);
    fe_copy(x3, x1);
    fe_1(z3);

    unsigned int swap = 0;
    for (int pos = 254; pos >= 0; --pos) {
        unsigned int b = (e[pos / 8] >> (pos & 7)) & 1;
        swap ^= b;
        fe_cswap(x2, x3, swap);
        fe_cswap(z2, z3, swap);
        swap = b;
        fe_sub(tmp0, x3, z3);
        fe_sub(tmp1, x2, z2);
        fe_add(x2, x2, z2);
        fe_add(z2, x3, z3);
        fe_mul(z3, tmp0, x2);
        fe_mul(z2, z2, tmp1);
        fe_sq(tmp0, tmp1);
        fe_sq(tmp1, x2);
        fe_add(x3, z3, z2);
        fe_sub(z2, z3, z2);
        fe_mul(x2, tmp1, tmp0);
        fe_sub(tmp1, tmp1, tmp0);
        fe_sq(z2, z2);
        fe_mul121666(z3, tmp1);
        fe_sq(x3, x3);
        fe_add(tmp0, tmp0, z3);
        fe_mul(z3, x1, z2);
        fe_mul(z2, tmp1, tmp0);
    }
    fe_cswap(x2, x3, swap);
    fe_cswap(z2, z3, swap);
    fe_invert(z2, z2);
    fe_mul(x2, x2, z2);
    fe_tobytes(out, x2);
}

void X25519PublicKey(uint8_t pub[32], const uint8_t priv[32]) {
    uint8_t base[32] = {9};
    X25519(pub, priv, base);
}

void Ed25519KeypairFromSeed(const uint8_t seed[32], uint8_t pub[32], uint8_t priv[64]) {
    ed25519_create_keypair(pub, priv, seed);
}

void Ed25519Sign(uint8_t sig[64], const uint8_t* msg, size_t n, const uint8_t pub[32], const uint8_t priv[64]) {
    ed25519_sign(sig, msg, n, pub, priv);
}

bool Ed25519Verify(const uint8_t sig[64], const uint8_t* msg, size_t n, const uint8_t pub[32]) {
    return ed25519_verify(sig, msg, n, pub) == 1;
}

// ---------------------------------------------------------------------------------------------
// AES via CNG

namespace {
BCRYPT_ALG_HANDLE AesAlg(const wchar_t* mode) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return nullptr;
    if (BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)mode, ULONG((wcslen(mode) + 1) * sizeof(wchar_t)),
                          0) != 0) {
        BCryptCloseAlgorithmProvider(alg, 0);
        return nullptr;
    }
    return alg;
}

BCRYPT_ALG_HANDLE EcbAlg() {
    static BCRYPT_ALG_HANDLE h = AesAlg(BCRYPT_CHAIN_MODE_ECB);
    return h;
}

BCRYPT_ALG_HANDLE CbcAlg() {
    static BCRYPT_ALG_HANDLE h = AesAlg(BCRYPT_CHAIN_MODE_CBC);
    return h;
}

void* MakeKey(BCRYPT_ALG_HANDLE alg, const uint8_t key[16]) {
    if (!alg) return nullptr;
    BCRYPT_KEY_HANDLE k = nullptr;
    if (BCryptGenerateSymmetricKey(alg, &k, nullptr, 0, (PUCHAR)key, 16, 0) != 0) return nullptr;
    return k;
}
}  // namespace

AesCtr::~AesCtr() {
    if (key_) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
}

bool AesCtr::Init(const uint8_t key[16], const uint8_t iv[16]) {
    if (key_) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
    key_ = MakeKey(EcbAlg(), key);
    std::memcpy(ctr_, iv, 16);
    in_.assign(16 * 256, 0);
    ks_.assign(16 * 256, 0);
    pos_ = ks_.size();  // force refill
    return key_ != nullptr;
}

void AesCtr::Refill() {
    const size_t blocks = in_.size() / 16;
    for (size_t b = 0; b < blocks; ++b) {
        std::memcpy(&in_[b * 16], ctr_, 16);
        for (int i = 15; i >= 0; --i) {
            if (++ctr_[i] != 0) break;
        }
    }
    ULONG done = 0;
    BCryptEncrypt(static_cast<BCRYPT_KEY_HANDLE>(key_), in_.data(), ULONG(in_.size()), nullptr, nullptr, 0,
                  ks_.data(), ULONG(ks_.size()), &done, 0);
    pos_ = 0;
}

void AesCtr::Process(uint8_t* data, size_t n) {
    if (!key_) return;
    while (n) {
        if (pos_ >= ks_.size()) Refill();
        size_t take = std::min(n, ks_.size() - pos_);
        const uint8_t* k = &ks_[pos_];
        for (size_t i = 0; i < take; ++i) data[i] ^= k[i];
        data += take;
        n -= take;
        pos_ += take;
    }
}

AesCbc::~AesCbc() {
    if (key_) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
}

bool AesCbc::Init(const uint8_t key[16]) {
    if (key_) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
    key_ = MakeKey(CbcAlg(), key);
    return key_ != nullptr;
}

bool AesCbc::Decrypt(const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t n) {
    if (!key_ || n % 16) return false;
    if (n == 0) return true;
    uint8_t ivc[16];
    std::memcpy(ivc, iv, 16);
    ULONG done = 0;
    return BCryptDecrypt(static_cast<BCRYPT_KEY_HANDLE>(key_), (PUCHAR)in, ULONG(n), nullptr, ivc, 16, out,
                         ULONG(n), &done, 0) == 0;
}

bool AesCbc::Encrypt(const uint8_t iv[16], const uint8_t* in, uint8_t* out, size_t n) {
    if (!key_ || n % 16) return false;
    if (n == 0) return true;
    uint8_t ivc[16];
    std::memcpy(ivc, iv, 16);
    ULONG done = 0;
    return BCryptEncrypt(static_cast<BCRYPT_KEY_HANDLE>(key_), (PUCHAR)in, ULONG(n), nullptr, ivc, 16, out,
                         ULONG(n), &done, 0) == 0;
}

// ---------------------------------------------------------------------------------------------
// Known-answer tests

static std::vector<uint8_t> H(const char* hex) {
    std::vector<uint8_t> v;
    HexDecode(hex, v);
    return v;
}

static bool Check(const char* what, const uint8_t* got, const std::vector<uint8_t>& want) {
    if (std::memcmp(got, want.data(), want.size()) == 0) return true;
    LOGE("selftest %s FAILED: got %s", what, HexEncode(got, want.size()).c_str());
    return false;
}

bool SelfTest() {
    bool ok = true;

    // SHA-512("abc")
    uint8_t h[64];
    Sha512Digest("abc", 3, h);
    ok &= Check("sha512", h,
                H("ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd"
                  "454d4423643ce80e2a9ac94fa54ca49f"));

    // RFC 7748 section 6.1
    auto aPriv = H("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
    auto bPriv = H("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
    uint8_t aPub[32], bPub[32], s1[32], s2[32];
    X25519PublicKey(aPub, aPriv.data());
    X25519PublicKey(bPub, bPriv.data());
    ok &= Check("x25519 pubA", aPub, H("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a"));
    ok &= Check("x25519 pubB", bPub, H("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f"));
    X25519(s1, aPriv.data(), bPub);
    X25519(s2, bPriv.data(), aPub);
    ok &= Check("x25519 shared1", s1, H("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"));
    ok &= Check("x25519 shared2", s2, H("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"));

    // RFC 8032 test 1 (empty message)
    auto seed = H("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60");
    uint8_t pub[32], priv[64], sig[64];
    Ed25519KeypairFromSeed(seed.data(), pub, priv);
    ok &= Check("ed25519 pub", pub, H("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a"));
    Ed25519Sign(sig, nullptr, 0, pub, priv);
    ok &= Check("ed25519 sig", sig,
                H("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46b"
                  "d25bf5f0595bbe24655141438e7a100b"));
    if (!Ed25519Verify(sig, nullptr, 0, pub)) {
        LOGE("selftest ed25519 verify FAILED");
        ok = false;
    }
    sig[5] ^= 1;
    if (Ed25519Verify(sig, nullptr, 0, pub)) {
        LOGE("selftest ed25519 verify accepted a bad signature");
        ok = false;
    }

    // NIST SP 800-38A F.5.1 CTR-AES128, split at odd offsets to exercise the continuous keystream
    auto key = H("2b7e151628aed2a6abf7158809cf4f3c");
    auto ctr = H("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
    auto pt = H("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
                "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
    auto ctWant = H("874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff"
                    "5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee");
    {
        AesCtr c;
        c.Init(key.data(), ctr.data());
        std::vector<uint8_t> buf = pt;
        c.Process(buf.data(), 7);
        c.Process(buf.data() + 7, 30);
        c.Process(buf.data() + 37, buf.size() - 37);
        ok &= Check("aes-ctr", buf.data(), ctWant);
    }

    // NIST SP 800-38A F.2.2 CBC-AES128 decrypt
    auto iv = H("000102030405060708090a0b0c0d0e0f");
    auto cbcCt = H("7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2"
                   "73bed6b8e3c1743b7116e69e222295163ff1caa1681fac09120eca307586e1a7");
    {
        AesCbc c;
        c.Init(key.data());
        std::vector<uint8_t> out(cbcCt.size());
        c.Decrypt(iv.data(), cbcCt.data(), out.data(), out.size());
        ok &= Check("aes-cbc", out.data(), pt);
    }

    LOGI("crypto self-test %s", ok ? "passed" : "FAILED");
    return ok;
}

}  // namespace crypto
