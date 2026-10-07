#include "net/bplist.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
uint64_t ReadBE(const uint8_t* p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v = (v << 8) | p[i];
    return v;
}

void AppendUtf8(std::string& s, uint32_t cp) {
    if (cp < 0x80) {
        s += char(cp);
    } else if (cp < 0x800) {
        s += char(0xC0 | (cp >> 6));
        s += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        s += char(0xE0 | (cp >> 12));
        s += char(0x80 | ((cp >> 6) & 0x3F));
        s += char(0x80 | (cp & 0x3F));
    } else {
        s += char(0xF0 | (cp >> 18));
        s += char(0x80 | ((cp >> 12) & 0x3F));
        s += char(0x80 | ((cp >> 6) & 0x3F));
        s += char(0x80 | (cp & 0x3F));
    }
}

// Decode UTF-8 into code points (lenient).
std::vector<uint32_t> Utf8Decode(const std::string& s) {
    std::vector<uint32_t> out;
    size_t i = 0;
    while (i < s.size()) {
        uint8_t c = uint8_t(s[i]);
        uint32_t cp;
        int extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { cp = 0xFFFD; extra = 0; }
        ++i;
        for (int k = 0; k < extra && i < s.size(); ++k, ++i) cp = (cp << 6) | (uint8_t(s[i]) & 0x3F);
        out.push_back(cp);
    }
    return out;
}
}  // namespace

bool Plist::AsBool(bool def) const {
    if (type_ == Type::Bool || type_ == Type::Int) return u_ != 0;
    return def;
}

uint64_t Plist::AsUInt(uint64_t def) const {
    if (type_ == Type::Int || type_ == Type::Bool || type_ == Type::Uid) return u_;
    if (type_ == Type::Real) return uint64_t(r_);
    return def;
}

double Plist::AsReal(double def) const {
    if (type_ == Type::Real || type_ == Type::Date) return r_;
    if (type_ == Type::Int) return double(int64_t(u_));
    return def;
}

const Plist* Plist::Get(const char* key) const {
    if (type_ != Type::Dict) return nullptr;
    for (auto& e : dict_)
        if (e.first == key) return &e.second;
    return nullptr;
}

Plist& Plist::Set(const std::string& key, Plist v) {
    type_ = Type::Dict;
    for (auto& e : dict_) {
        if (e.first == key) {
            e.second = std::move(v);
            return *this;
        }
    }
    dict_.emplace_back(key, std::move(v));
    return *this;
}

Plist& Plist::Append(Plist v) {
    type_ = Type::Array;
    arr_.push_back(std::move(v));
    return *this;
}

std::string Plist::Describe(int depth) const {
    char buf[64];
    switch (type_) {
    case Type::Null: return "null";
    case Type::Bool: return u_ ? "true" : "false";
    case Type::Int: snprintf(buf, sizeof(buf), "%llu", (unsigned long long)u_); return buf;
    case Type::Uid: snprintf(buf, sizeof(buf), "uid(%llu)", (unsigned long long)u_); return buf;
    case Type::Real: snprintf(buf, sizeof(buf), "%g", r_); return buf;
    case Type::Date: snprintf(buf, sizeof(buf), "date(%g)", r_); return buf;
    case Type::String: return "\"" + s_ + "\"";
    case Type::Data: snprintf(buf, sizeof(buf), "<%zu bytes>", data_.size()); return buf;
    case Type::Array: {
        if (depth > 6) return "[...]";
        std::string s = "[";
        for (size_t i = 0; i < arr_.size(); ++i) {
            if (i) s += ", ";
            s += arr_[i].Describe(depth + 1);
        }
        return s + "]";
    }
    case Type::Dict: {
        if (depth > 6) return "{...}";
        std::string s = "{";
        for (size_t i = 0; i < dict_.size(); ++i) {
            if (i) s += ", ";
            s += dict_[i].first + ": " + dict_[i].second.Describe(depth + 1);
        }
        return s + "}";
    }
    }
    return "?";
}

// ---------------------------------------------------------------------------------------------

class BplistReader {
public:
    BplistReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}

    bool Read(Plist& out) {
        if (n_ < 8 + 32 || std::memcmp(p_, "bplist00", 8) != 0) return false;
        const uint8_t* t = p_ + n_ - 32;
        offSize_ = t[6];
        refSize_ = t[7];
        numObjects_ = ReadBE(t + 8, 8);
        uint64_t top = ReadBE(t + 16, 8);
        offTable_ = ReadBE(t + 24, 8);
        if (offSize_ < 1 || offSize_ > 8 || refSize_ < 1 || refSize_ > 8) return false;
        if (numObjects_ == 0 || numObjects_ > 1000000 || top >= numObjects_) return false;
        if (offTable_ < 8 || offTable_ + numObjects_ * offSize_ > n_ - 32) return false;
        visiting_.assign(size_t(numObjects_), false);
        return ReadObject(top, out, 0);
    }

private:
    bool ObjectOffset(uint64_t idx, uint64_t& off) {
        if (idx >= numObjects_) return false;
        off = ReadBE(p_ + offTable_ + idx * offSize_, offSize_);
        return off >= 8 && off < offTable_;
    }

    // Reads a length that follows a marker whose low nibble is 0xF.
    bool ReadLength(uint8_t low, uint64_t& pos, uint64_t& len) {
        if (low != 0xF) {
            len = low;
            return true;
        }
        if (pos >= offTable_) return false;
        uint8_t m = p_[pos++];
        if ((m & 0xF0) != 0x10) return false;
        size_t sz = size_t(1) << (m & 0xF);
        if (sz > 8 || pos + sz > offTable_) return false;
        len = ReadBE(p_ + pos, sz);
        pos += sz;
        return true;
    }

    bool ReadObject(uint64_t idx, Plist& out, int depth) {
        if (depth > 32) return false;
        uint64_t pos;
        if (!ObjectOffset(idx, pos)) return false;
        if (visiting_[size_t(idx)]) return false;  // cycle
        uint8_t m = p_[pos++];
        uint8_t hi = m >> 4, lo = m & 0xF;
        switch (hi) {
        case 0x0:
            if (m == 0x08 || m == 0x09) {
                out = Plist::Bool(m == 0x09);
            } else {
                out = Plist();
            }
            return true;
        case 0x1: {
            size_t sz = size_t(1) << lo;
            if (sz > 16 || pos + sz > offTable_) return false;
            // 16-byte ints carry large unsigned values; keep the low 64 bits.
            uint64_t v = sz == 16 ? ReadBE(p_ + pos + 8, 8) : ReadBE(p_ + pos, sz);
            out = Plist::Int(v);
            return true;
        }
        case 0x2: {
            size_t sz = size_t(1) << lo;
            if (pos + sz > offTable_) return false;
            if (sz == 4) {
                uint32_t u = uint32_t(ReadBE(p_ + pos, 4));
                float f;
                std::memcpy(&f, &u, 4);
                out = Plist::Real(f);
            } else if (sz == 8) {
                uint64_t u = ReadBE(p_ + pos, 8);
                double d;
                std::memcpy(&d, &u, 8);
                out = Plist::Real(d);
            } else {
                return false;
            }
            return true;
        }
        case 0x3: {
            if (m != 0x33 || pos + 8 > offTable_) return false;
            uint64_t u = ReadBE(p_ + pos, 8);
            double d;
            std::memcpy(&d, &u, 8);
            out = Plist::Real(d);
            out.type_ = Plist::Type::Date;
            return true;
        }
        case 0x4: {
            uint64_t len;
            if (!ReadLength(lo, pos, len) || pos + len > offTable_) return false;
            out = Plist::Data(p_ + pos, size_t(len));
            return true;
        }
        case 0x5: {
            uint64_t len;
            if (!ReadLength(lo, pos, len) || pos + len > offTable_) return false;
            out = Plist::String(std::string(reinterpret_cast<const char*>(p_ + pos), size_t(len)));
            return true;
        }
        case 0x6: {
            uint64_t len;
            if (!ReadLength(lo, pos, len) || pos + len * 2 > offTable_) return false;
            std::string s;
            for (uint64_t i = 0; i < len; ++i) {
                uint32_t c = uint32_t(ReadBE(p_ + pos + i * 2, 2));
                if (c >= 0xD800 && c < 0xDC00 && i + 1 < len) {
                    uint32_t c2 = uint32_t(ReadBE(p_ + pos + (i + 1) * 2, 2));
                    if (c2 >= 0xDC00 && c2 < 0xE000) {
                        c = 0x10000 + ((c - 0xD800) << 10) + (c2 - 0xDC00);
                        ++i;
                    }
                }
                AppendUtf8(s, c);
            }
            out = Plist::String(std::move(s));
            return true;
        }
        case 0x8: {
            size_t sz = size_t(lo) + 1;
            if (pos + sz > offTable_) return false;
            out = Plist::Int(ReadBE(p_ + pos, std::min<size_t>(sz, 8)));
            out.type_ = Plist::Type::Uid;
            return true;
        }
        case 0xA: {
            uint64_t len;
            if (!ReadLength(lo, pos, len) || pos + len * refSize_ > offTable_) return false;
            out = Plist::Array();
            visiting_[size_t(idx)] = true;
            for (uint64_t i = 0; i < len; ++i) {
                Plist item;
                if (!ReadObject(ReadBE(p_ + pos + i * refSize_, refSize_), item, depth + 1)) return false;
                out.arr_.push_back(std::move(item));
            }
            visiting_[size_t(idx)] = false;
            return true;
        }
        case 0xD: {
            uint64_t len;
            if (!ReadLength(lo, pos, len) || pos + len * 2 * refSize_ > offTable_) return false;
            out = Plist::Dict();
            visiting_[size_t(idx)] = true;
            for (uint64_t i = 0; i < len; ++i) {
                Plist k, v;
                if (!ReadObject(ReadBE(p_ + pos + i * refSize_, refSize_), k, depth + 1)) return false;
                if (!ReadObject(ReadBE(p_ + pos + (len + i) * refSize_, refSize_), v, depth + 1)) return false;
                if (k.type() != Plist::Type::String) return false;
                out.dict_.emplace_back(k.s_, std::move(v));
            }
            visiting_[size_t(idx)] = false;
            return true;
        }
        default:
            return false;
        }
    }

    const uint8_t* p_;
    size_t n_;
    uint8_t offSize_ = 0, refSize_ = 0;
    uint64_t numObjects_ = 0, offTable_ = 0;
    std::vector<bool> visiting_;
};

bool Plist::Parse(const uint8_t* p, size_t n, Plist& out) {
    if (!p) return false;
    BplistReader r(p, n);
    return r.Read(out);
}

// ---------------------------------------------------------------------------------------------

class BplistWriter {
public:
    std::vector<uint8_t> Write(const Plist& root) {
        // Flatten into a list of objects; index 0 is the root.
        objs_.clear();
        Flatten(root);
        size_t count = objs_.size() + keyCount_;
        refSize_ = count < 256 ? 1 : (count < 65536 ? 2 : 4);

        out_.assign({'b', 'p', 'l', 'i', 's', 't', '0', '0'});
        offsets_.clear();
        nextIndex_ = 0;
        WriteObject(root);

        uint64_t offTable = out_.size();
        uint64_t maxOff = 0;
        for (uint64_t o : offsets_) maxOff = std::max(maxOff, o);
        uint8_t offSize = maxOff < 256 ? 1 : (maxOff < 65536 ? 2 : (maxOff < (1ull << 32) ? 4 : 8));
        for (uint64_t o : offsets_) PutBE(o, offSize);
        uint8_t trailer[32] = {};
        trailer[6] = offSize;
        trailer[7] = uint8_t(refSize_);
        uint64_t num = offsets_.size();
        for (int i = 0; i < 8; ++i) {
            trailer[8 + i] = uint8_t(num >> (56 - 8 * i));
            trailer[16 + i] = 0;  // top object is index 0
            trailer[24 + i] = uint8_t(offTable >> (56 - 8 * i));
        }
        out_.insert(out_.end(), trailer, trailer + 32);
        return std::move(out_);
    }

private:
    void Flatten(const Plist& p) {
        objs_.push_back(&p);
        if (p.type() == Plist::Type::Array) {
            for (auto& i : p.arr_) Flatten(i);
        } else if (p.type() == Plist::Type::Dict) {
            keyCount_ += p.dict_.size();
            for (auto& e : p.dict_) Flatten(e.second);
        }
    }

    void PutBE(uint64_t v, size_t n) {
        for (size_t i = 0; i < n; ++i) out_.push_back(uint8_t(v >> (8 * (n - 1 - i))));
    }

    void PutMarker(uint8_t hi, uint64_t len) {
        if (len < 15) {
            out_.push_back(uint8_t((hi << 4) | len));
        } else {
            out_.push_back(uint8_t((hi << 4) | 0xF));
            PutInt(len);
        }
    }

    void PutInt(uint64_t v) {
        if (v <= 0xFF) { out_.push_back(0x10); PutBE(v, 1); }
        else if (v <= 0xFFFF) { out_.push_back(0x11); PutBE(v, 2); }
        else if (v <= 0xFFFFFFFFull) { out_.push_back(0x12); PutBE(v, 4); }
        else if (v <= 0x7FFFFFFFFFFFFFFFull) { out_.push_back(0x13); PutBE(v, 8); }
        else { out_.push_back(0x14); PutBE(0, 8); PutBE(v, 8); }
    }

    void PutString(const std::string& s) {
        bool ascii = true;
        for (unsigned char c : s)
            if (c >= 0x80) { ascii = false; break; }
        if (ascii) {
            PutMarker(0x5, s.size());
            out_.insert(out_.end(), s.begin(), s.end());
        } else {
            std::vector<uint16_t> u16;
            for (uint32_t cp : Utf8Decode(s)) {
                if (cp >= 0x10000) {
                    cp -= 0x10000;
                    u16.push_back(uint16_t(0xD800 + (cp >> 10)));
                    u16.push_back(uint16_t(0xDC00 + (cp & 0x3FF)));
                } else {
                    u16.push_back(uint16_t(cp));
                }
            }
            PutMarker(0x6, u16.size());
            for (uint16_t c : u16) PutBE(c, 2);
        }
    }

    // Writes the object and its children depth-first; returns its index.
    uint64_t WriteObject(const Plist& p) {
        uint64_t idx = nextIndex_++;
        offsets_.resize(nextIndex_);
        switch (p.type()) {
        case Plist::Type::Array: {
            // Children first so their indices are known; the container itself is written last.
            std::vector<uint64_t> refs;
            for (auto& c : p.arr_) refs.push_back(WriteObject(c));
            offsets_[idx] = out_.size();
            PutMarker(0xA, refs.size());
            for (uint64_t r : refs) PutBE(r, refSize_);
            return idx;
        }
        case Plist::Type::Dict: {
            std::vector<uint64_t> keyRefs, valRefs;
            for (auto& e : p.dict_) {
                uint64_t k = nextIndex_++;
                offsets_.resize(nextIndex_);
                offsets_[k] = out_.size();
                PutString(e.first);
                keyRefs.push_back(k);
                valRefs.push_back(WriteObject(e.second));
            }
            offsets_[idx] = out_.size();
            PutMarker(0xD, keyRefs.size());
            for (uint64_t r : keyRefs) PutBE(r, refSize_);
            for (uint64_t r : valRefs) PutBE(r, refSize_);
            return idx;
        }
        default:
            break;
        }
        offsets_[idx] = out_.size();
        switch (p.type()) {
        case Plist::Type::Null: out_.push_back(0x00); break;
        case Plist::Type::Bool: out_.push_back(p.u_ ? 0x09 : 0x08); break;
        case Plist::Type::Int: PutInt(p.u_); break;
        case Plist::Type::Uid: out_.push_back(0x80 | 7); PutBE(p.u_, 8); break;
        case Plist::Type::Real:
        case Plist::Type::Date: {
            out_.push_back(p.type() == Plist::Type::Date ? 0x33 : 0x23);
            uint64_t u;
            std::memcpy(&u, &p.r_, 8);
            PutBE(u, 8);
            break;
        }
        case Plist::Type::String: PutString(p.s_); break;
        case Plist::Type::Data:
            PutMarker(0x4, p.data_.size());
            out_.insert(out_.end(), p.data_.begin(), p.data_.end());
            break;
        default: break;
        }
        return idx;
    }

    std::vector<const Plist*> objs_;
    size_t keyCount_ = 0;
    size_t refSize_ = 1;
    uint64_t nextIndex_ = 0;
    std::vector<uint64_t> offsets_;
    std::vector<uint8_t> out_;
};

std::vector<uint8_t> Plist::Serialize() const {
    BplistWriter w;
    return w.Write(*this);
}
