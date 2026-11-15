#include "crypto/srp.h"

#include <cstring>
#include <initializer_list>

#include "crypto/crypto.h"

namespace crypto::srp {
namespace {

// ---------------------------------------------------------------------------------------------
// Unsigned big integers: little-endian 32-bit limbs, no leading zero limbs (empty = 0). Only what SRP needs, with
// Montgomery multiplication for the modular exponentiations.

using Limbs = std::vector<uint32_t>;

void Trim(Limbs& a) {
    while (!a.empty() && a.back() == 0) a.pop_back();
}

Limbs FromBytes(const Bytes& b) {
    Limbs r((b.size() + 3) / 4, 0);
    for (size_t i = 0; i < b.size(); ++i) r[i / 4] |= uint32_t(b[b.size() - 1 - i]) << (8 * (i % 4));
    Trim(r);
    return r;
}

// Big-endian, minimal length, or left-padded with zeros to `len` when that is longer.
Bytes ToBytes(const Limbs& a, size_t len = 0) {
    size_t n = a.size() * 4;
    while (n > 0 && ((a[(n - 1) / 4] >> (8 * ((n - 1) % 4))) & 0xFF) == 0) --n;
    size_t out = std::max(n, len);
    Bytes r(out, 0);
    for (size_t i = 0; i < n; ++i) r[out - 1 - i] = uint8_t(a[i / 4] >> (8 * (i % 4)));
    return r;
}

int Cmp(const Limbs& a, const Limbs& b) {
    if (a.size() != b.size()) return a.size() < b.size() ? -1 : 1;
    for (size_t i = a.size(); i-- > 0;)
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    return 0;
}

Limbs Add(const Limbs& a, const Limbs& b) {
    Limbs r(std::max(a.size(), b.size()) + 1, 0);
    uint64_t c = 0;
    for (size_t i = 0; i + 1 < r.size(); ++i) {
        c += uint64_t(i < a.size() ? a[i] : 0) + (i < b.size() ? b[i] : 0);
        r[i] = uint32_t(c);
        c >>= 32;
    }
    r.back() = uint32_t(c);
    Trim(r);
    return r;
}

void SubInPlace(Limbs& a, const Limbs& b) {  // requires a >= b
    uint64_t borrow = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        uint64_t d = uint64_t(a[i]) - (i < b.size() ? b[i] : 0) - borrow;
        a[i] = uint32_t(d);
        borrow = (d >> 32) & 1;
    }
    Trim(a);
}

Limbs Mul(const Limbs& a, const Limbs& b) {
    if (a.empty() || b.empty()) return {};
    Limbs r(a.size() + b.size(), 0);
    for (size_t i = 0; i < a.size(); ++i) {
        uint64_t c = 0;
        for (size_t j = 0; j < b.size(); ++j) {
            uint64_t t = uint64_t(a[i]) * b[j] + r[i + j] + c;
            r[i + j] = uint32_t(t);
            c = t >> 32;
        }
        r[i + b.size()] = uint32_t(c);
    }
    Trim(r);
    return r;
}

// a mod m by binary long division (m != 0). Only used on public values and once per modulus.
Limbs Mod(const Limbs& a, const Limbs& m) {
    Limbs r;
    for (size_t i = a.size() * 32; i-- > 0;) {
        uint32_t carry = (a[i / 32] >> (i % 32)) & 1;
        for (auto& w : r) {
            uint32_t top = w >> 31;
            w = (w << 1) | carry;
            carry = top;
        }
        if (carry) r.push_back(carry);
        if (Cmp(r, m) >= 0) SubInPlace(r, m);
    }
    return r;
}

// Montgomery arithmetic modulo an odd n of k limbs (R = 2^(32k)). Values are kept at exactly k limbs.
class Mont {
public:
    explicit Mont(const Limbs& n) : n_(n), k_(n.size()) {
        uint32_t inv = 1;  // n[0]^-1 mod 2^32 by Newton's iteration (n is odd)
        for (int i = 0; i < 5; ++i) inv *= 2 - n_[0] * inv;
        n0_ = 0u - inv;
        Limbs r2(2 * k_ + 1, 0);
        r2[2 * k_] = 1;
        r2_ = Pad(Mod(r2, n_));
    }

    // base^e mod n. The exponent's bits pick results with masks rather than branches.
    Limbs Pow(const Limbs& base, const Limbs& e) const {
        Limbs one(k_, 0);
        one[0] = 1;
        Limbs b = MulMont(Pad(Mod(base, n_)), r2_);
        Limbs acc = MulMont(one, r2_);  // 1 in Montgomery form
        for (size_t i = e.size() * 32; i-- > 0;) {
            acc = MulMont(acc, acc);
            Limbs t = MulMont(acc, b);
            uint32_t mask = 0u - ((e[i / 32] >> (i % 32)) & 1);
            for (size_t j = 0; j < k_; ++j) acc[j] = (t[j] & mask) | (acc[j] & ~mask);
        }
        Limbs r = MulMont(acc, one);
        Trim(r);
        return r;
    }

private:
    Limbs Pad(Limbs a) const {
        a.resize(k_, 0);
        return a;
    }

    // a * b * R^-1 mod n for a, b < n (CIOS).
    Limbs MulMont(const Limbs& a, const Limbs& b) const {
        std::vector<uint32_t> t(k_ + 2, 0);
        for (size_t i = 0; i < k_; ++i) {
            uint64_t c = 0;
            for (size_t j = 0; j < k_; ++j) {
                uint64_t s = uint64_t(t[j]) + uint64_t(a[j]) * b[i] + c;
                t[j] = uint32_t(s);
                c = s >> 32;
            }
            uint64_t s = uint64_t(t[k_]) + c;
            t[k_] = uint32_t(s);
            t[k_ + 1] = uint32_t(s >> 32);
            uint32_t m = t[0] * n0_;
            s = uint64_t(t[0]) + uint64_t(m) * n_[0];
            c = s >> 32;
            for (size_t j = 1; j < k_; ++j) {
                s = uint64_t(t[j]) + uint64_t(m) * n_[j] + c;
                t[j - 1] = uint32_t(s);
                c = s >> 32;
            }
            s = uint64_t(t[k_]) + c;
            t[k_ - 1] = uint32_t(s);
            t[k_] = t[k_ + 1] + uint32_t(s >> 32);
        }
        // t < 2n: subtract n once if t >= n, chosen by mask.
        Limbs r(t.begin(), t.begin() + long(k_)), d(k_);
        uint64_t borrow = 0;
        for (size_t j = 0; j < k_; ++j) {
            uint64_t x = uint64_t(r[j]) - n_[j] - borrow;
            d[j] = uint32_t(x);
            borrow = (x >> 32) & 1;
        }
        uint32_t mask = 0u - uint32_t(t[k_] != 0 || borrow == 0);
        for (size_t j = 0; j < k_; ++j) r[j] = (d[j] & mask) | (r[j] & ~mask);
        return r;
    }

    Limbs n_;
    size_t k_;
    uint32_t n0_ = 0;
    Limbs r2_;
};

Limbs PowMod(const Limbs& base, const Limbs& e, const Limbs& n) { return Mont(n).Pow(base, e); }

// ---------------------------------------------------------------------------------------------

Bytes Cat(std::initializer_list<const Bytes*> parts) {
    Bytes r;
    for (const Bytes* p : parts) r.insert(r.end(), p->begin(), p->end());
    return r;
}

Bytes Minimal(const Bytes& b) { return ToBytes(FromBytes(b)); }
Bytes Padded(const Bytes& b, size_t len) { return ToBytes(FromBytes(b), len); }

void Wipe(Bytes& b) {
    volatile uint8_t* p = b.data();
    for (size_t i = 0; i < b.size(); ++i) p[i] = 0;
    b.clear();
}

Bytes FromHex(const char* hex) {
    Bytes r;
    for (const char* p = hex; p[0] && p[1]; p += 2) {
        auto nib = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
        r.push_back(uint8_t((nib(p[0]) << 4) | nib(p[1])));
    }
    return r;
}

}  // namespace

const Group& Group1024() {
    static const Group g{FromHex("EEAF0AB9ADB38DD69C33F80AFA8FC5E86072618775FF3C0B9EA2314C9C256576D674DF7496EA81D3383B"
                                 "4813D692C6E0E0D5D8E250B98BE48E495C1D6089DAD15DC7D7B46154D6B6CE8EF4AD69B15D4982559B29"
                                 "7BCF1885C529F566660E57EC68EDBC3C05726CC02FD4CBF4976EAA9AFD5138FE8376435B9FC61D2FC0EB"
                                 "06E3"),
                         Bytes{2}};
    return g;
}

const Group& Group2048() {
    static const Group g{FromHex("AC6BDB41324A9A9BF166DE5E1389582FAF72B6651987EE07FC3192943DB56050A37329CBB4A099ED8193"
                                 "E0757767A13DD52312AB4B03310DCD7F48A9DA04FD50E8083969EDB767B0CF6095179A163AB3661A05FB"
                                 "D5FAAAE82918A9962F0B93B855F97993EC975EEAA80D740ADBF4FF747359D041D5C33EA71D281E446B14"
                                 "773BCA97B43A23FB801676BD207A436C6481F1D2B9078717461A5B9D32E688F87748544523B524B0D57D"
                                 "5EA77A2775D2ECFA032CFBDBF52FB3786160279004E57AE6AF874E7303CE53299CCC041C7BC308D82A56"
                                 "98F3A8D0C38271AE35F8E9DBFBB694B5C803D89F7AE435DE236D525F54759B65E372FCD68EF20FA7111F"
                                 "9E4AFF73"),
                         Bytes{2}};
    return g;
}

Bytes HashSha1(const Bytes& data) {
    Bytes h(20);
    Sha1Digest(data.data(), data.size(), h.data());
    return h;
}

Bytes ModPow(const Bytes& base, const Bytes& exp, const Bytes& mod) {
    return ToBytes(PowMod(FromBytes(base), FromBytes(exp), FromBytes(mod)));
}

Bytes Multiplier(const Group& gr) {
    Bytes g = Padded(gr.g, gr.N.size());
    return Minimal(HashSha1(Cat({&gr.N, &g})));
}

Bytes PrivateX(const Bytes& salt, const std::string& user, const std::string& pass) {
    std::string ip = user + ":" + pass;
    Bytes inner = HashSha1(Bytes(ip.begin(), ip.end()));
    SecureWipe(ip);
    Bytes x = Minimal(HashSha1(Cat({&salt, &inner})));
    Wipe(inner);
    return x;
}

Bytes Verifier(const Group& gr, const Bytes& x) { return ModPow(gr.g, x, gr.N); }

Bytes ClientPublic(const Group& gr, const Bytes& a) { return ModPow(gr.g, a, gr.N); }

Bytes ServerPublic(const Group& gr, const Bytes& v, const Bytes& b) {
    Limbs N = FromBytes(gr.N);
    Limbs kv = Mod(Mul(FromBytes(Multiplier(gr)), FromBytes(v)), N);
    return ToBytes(Mod(Add(kv, PowMod(FromBytes(gr.g), FromBytes(b), N)), N));
}

Bytes Scrambler(const Group& gr, const Bytes& A, const Bytes& B) {
    Bytes a = Padded(A, gr.N.size()), b = Padded(B, gr.N.size());
    return Minimal(HashSha1(Cat({&a, &b})));
}

Bytes ServerSecret(const Group& gr, const Bytes& A, const Bytes& v, const Bytes& u, const Bytes& b) {
    Limbs N = FromBytes(gr.N);
    Limbs avu = Mod(Mul(FromBytes(A), PowMod(FromBytes(v), FromBytes(u), N)), N);
    return ToBytes(PowMod(avu, FromBytes(b), N));
}

Bytes ClientSecret(const Group& gr, const Bytes& B, const Bytes& x, const Bytes& a, const Bytes& u) {
    Limbs N = FromBytes(gr.N);
    Limbs kgx = Mod(Mul(FromBytes(Multiplier(gr)), PowMod(FromBytes(gr.g), FromBytes(x), N)), N);
    Limbs base = Mod(FromBytes(B), N);
    if (Cmp(base, kgx) < 0) base = Add(base, N);
    SubInPlace(base, kgx);
    Limbs e = Add(FromBytes(a), Mul(FromBytes(u), FromBytes(x)));
    return ToBytes(PowMod(base, e, N));
}

Bytes SessionKey(const Bytes& S) {
    Bytes s = Minimal(S);
    Bytes K;
    for (uint8_t i = 0; i < 2; ++i) {
        Bytes counter = {0, 0, 0, i};
        Bytes h = HashSha1(Cat({&s, &counter}));
        K.insert(K.end(), h.begin(), h.end());
    }
    Wipe(s);
    return K;
}

Bytes ClientProof(const Group& gr, const std::string& user, const Bytes& salt, const Bytes& A, const Bytes& B,
                  const Bytes& K) {
    Bytes hn = HashSha1(Minimal(gr.N)), hg = HashSha1(Minimal(gr.g));
    for (size_t i = 0; i < hn.size(); ++i) hn[i] ^= hg[i];
    Bytes hi = HashSha1(Bytes(user.begin(), user.end()));
    Bytes s = Minimal(salt), a = Minimal(A), b = Minimal(B);
    return HashSha1(Cat({&hn, &hi, &s, &a, &b, &K}));
}

Bytes ServerProof(const Bytes& A, const Bytes& M1, const Bytes& K) {
    Bytes a = Minimal(A);
    return HashSha1(Cat({&a, &M1, &K}));
}

// ---------------------------------------------------------------------------------------------

Server::~Server() {
    Wipe(v_);
    Wipe(b_);
    Wipe(K_);
}

void Server::Start(const std::string& user, const std::string& pin, const Bytes& salt, const Bytes& b) {
    const Group& gr = Group2048();
    user_ = user;
    salt_ = salt;
    if (salt_.empty()) {
        salt_.assign(16, 0);
        do RandomBytes(salt_.data(), salt_.size());
        while (salt_[0] == 0);
    }
    Bytes x = PrivateX(salt_, user, pin);
    v_ = Verifier(gr, x);
    Wipe(x);
    for (;;) {
        b_ = b;
        if (b_.empty()) {
            b_.assign(32, 0);
            RandomBytes(b_.data(), b_.size());
        }
        B_ = ServerPublic(gr, v_, b_);
        if (B_.size() == gr.N.size() || !b.empty()) break;  // a B with a leading zero byte: pick another b
    }
    Wipe(K_);
}

bool Server::Verify(const Bytes& A, const Bytes& M1, Bytes& M2) {
    const Group& gr = Group2048();
    if (!Started() || b_.empty()) return false;
    // One try per Start, whatever happens.
    Bytes b = std::move(b_);
    b_.clear();
    bool ok = false;
    Limbs a = FromBytes(A);
    Bytes u = Scrambler(gr, A, B_);
    // SRP-6a safety checks: A mod N != 0 and u != 0.
    if (A.size() <= gr.N.size() && !Mod(a, FromBytes(gr.N)).empty() && !FromBytes(u).empty() && M1.size() == 20) {
        Bytes S = ServerSecret(gr, A, v_, u, b);
        Bytes K = SessionKey(S);
        Wipe(S);
        Bytes want = ClientProof(gr, user_, salt_, A, B_, K);
        uint8_t diff = 0;
        for (size_t i = 0; i < 20; ++i) diff |= uint8_t(want[i] ^ M1[i]);
        if (diff == 0) {
            M2 = ServerProof(A, M1, K);
            K_ = std::move(K);
            ok = true;
        } else {
            Wipe(K);
        }
    }
    Wipe(b);
    Wipe(v_);
    return ok;
}

bool Client::Respond(const std::string& user, const std::string& pin, const Bytes& salt, const Bytes& B, Bytes& A,
                     Bytes& M1, const Bytes& a) {
    const Group& gr = Group2048();
    if (B.size() > gr.N.size() || Mod(FromBytes(B), FromBytes(gr.N)).empty()) return false;
    Bytes priv = a;
    if (priv.empty()) {
        priv.assign(32, 0);
        RandomBytes(priv.data(), priv.size());
    }
    A = ClientPublic(gr, priv);
    Bytes u = Scrambler(gr, A, B);
    if (FromBytes(u).empty()) return false;
    Bytes x = PrivateX(salt, user, pin);
    Bytes S = ClientSecret(gr, B, x, priv, u);
    K_ = SessionKey(S);
    M1 = ClientProof(gr, user, salt, A, B, K_);
    M2_ = ServerProof(A, M1, K_);
    Wipe(S);
    Wipe(x);
    Wipe(priv);
    return true;
}

void PinSetupCipher(const Bytes& K, uint8_t key[16], uint8_t nonce[16]) {
    uint8_t d[64];
    for (int i = 0; i < 2; ++i) {
        const char* salt = i == 0 ? "Pair-Setup-AES-Key" : "Pair-Setup-AES-IV";
        Sha512 h;
        h.Update(salt, strlen(salt));
        h.Update(K.data(), K.size());
        h.Final(d);
        std::memcpy(i == 0 ? key : nonce, d, 16);
    }
    SecureWipe(d, sizeof(d));
}

}  // namespace crypto::srp
