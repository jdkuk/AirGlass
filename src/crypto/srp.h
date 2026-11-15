// SRP-6a (RFC 5054) as AirPlay's legacy PIN pairing ("AirPlay Code") uses it: SHA-1, the 2048-bit group, a 16-byte
// salt, and Apple's 40-byte session key K = H(S | 00000000) | H(S | 00000001). The same variant as UxPlay's
// lib/srp.c (csrp, "APPLE_VARIANT") and pyatv. Big numbers are big-endian byte strings.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace crypto::srp {

using Bytes = std::vector<uint8_t>;

struct Group {
    Bytes N;  // prime modulus
    Bytes g;  // generator
};
const Group& Group1024();  // RFC 5054 appendix A.1 (its test vectors use this one)
const Group& Group2048();  // RFC 5054 appendix A.3 (AirPlay)

// The steps of the protocol, exposed for the known-answer tests. Results are minimal-length (no leading zeros)
// unless said otherwise; inputs may have leading zeros.
Bytes HashSha1(const Bytes& data);
Bytes Multiplier(const Group& gr);                                                        // k = H(N | PAD(g))
Bytes PrivateX(const Bytes& salt, const std::string& user, const std::string& pass);     // x = H(s | H(I ":" P))
Bytes Verifier(const Group& gr, const Bytes& x);                                         // v = g^x
Bytes ClientPublic(const Group& gr, const Bytes& a);                                     // A = g^a
Bytes ServerPublic(const Group& gr, const Bytes& v, const Bytes& b);                     // B = kv + g^b
Bytes Scrambler(const Group& gr, const Bytes& A, const Bytes& B);                        // u = H(PAD(A) | PAD(B))
Bytes ServerSecret(const Group& gr, const Bytes& A, const Bytes& v, const Bytes& u, const Bytes& b);  // (A v^u)^b
Bytes ClientSecret(const Group& gr, const Bytes& B, const Bytes& x, const Bytes& a, const Bytes& u);  // (B - kg^x)^(a+ux)
Bytes SessionKey(const Bytes& S);                                                        // Apple: 40 bytes
Bytes ClientProof(const Group& gr, const std::string& user, const Bytes& salt, const Bytes& A, const Bytes& B,
                  const Bytes& K);                                                       // M1 (20 bytes)
Bytes ServerProof(const Bytes& A, const Bytes& M1, const Bytes& K);                     // M2 (20 bytes)
Bytes ModPow(const Bytes& base, const Bytes& exp, const Bytes& mod);                     // mod must be odd

// The receiver's side of one PIN attempt, in the 2048-bit group.
class Server {
public:
    // A fresh salt and ephemeral key for `user` (the sender's id) and `pin`. Salt (16 bytes) and B (256 bytes)
    // never start with a zero byte, so the padded and minimal encodings agree. Tests pass salt and b.
    void Start(const std::string& user, const std::string& pin, const Bytes& salt = {}, const Bytes& b = {});
    const Bytes& Salt() const { return salt_; }
    const Bytes& PublicKey() const { return B_; }
    // Checks the sender's public key A and proof M1. On success fills M2 (20 bytes) and SessionKey().
    // Each Start allows one Verify: the code is tried once.
    bool Verify(const Bytes& A, const Bytes& M1, Bytes& M2);
    const Bytes& Key() const { return K_; }
    bool Started() const { return !B_.empty(); }
    ~Server();

private:
    std::string user_;
    Bytes salt_, v_, b_, B_, K_;
};

// The sender's side (used by the tests and the loopback PIN test).
class Client {
public:
    // Computes A and M1 for the server's salt and B. False if B is unusable. Tests pass a.
    bool Respond(const std::string& user, const std::string& pin, const Bytes& salt, const Bytes& B, Bytes& A,
                 Bytes& M1, const Bytes& a = {});
    bool CheckServer(const Bytes& M2) const { return !M2_.empty() && M2 == M2_; }
    const Bytes& Key() const { return K_; }

private:
    Bytes K_, M2_;
};

// pair-setup-pin step 3: both sides send their Ed25519 public key, AES-128-GCM-encrypted with a key and a 16-byte
// nonce made from the session key (SHA-512 of "Pair-Setup-AES-Key" / "Pair-Setup-AES-IV" + K). The nonce's last
// byte goes up by one before the sender's message and again before the receiver's answer.
void PinSetupCipher(const Bytes& K, uint8_t key[16], uint8_t nonce[16]);

}  // namespace crypto::srp
