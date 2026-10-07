// Binary property list ("bplist00") reader and writer.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class Plist {
public:
    enum class Type { Null, Bool, Int, Real, Date, String, Data, Array, Dict, Uid };

    Plist() = default;
    static Plist Bool(bool b) { Plist p; p.type_ = Type::Bool; p.u_ = b ? 1 : 0; return p; }
    static Plist Int(uint64_t v) { Plist p; p.type_ = Type::Int; p.u_ = v; return p; }
    static Plist Real(double d) { Plist p; p.type_ = Type::Real; p.r_ = d; return p; }
    static Plist String(std::string s) { Plist p; p.type_ = Type::String; p.s_ = std::move(s); return p; }
    static Plist Data(std::vector<uint8_t> d) { Plist p; p.type_ = Type::Data; p.data_ = std::move(d); return p; }
    static Plist Data(const uint8_t* d, size_t n) { return Data(std::vector<uint8_t>(d, d + n)); }
    static Plist Array() { Plist p; p.type_ = Type::Array; return p; }
    static Plist Dict() { Plist p; p.type_ = Type::Dict; return p; }

    Type type() const { return type_; }
    bool IsDict() const { return type_ == Type::Dict; }
    bool IsArray() const { return type_ == Type::Array; }

    // Accessors (return defaults on type mismatch).
    bool AsBool(bool def = false) const;
    uint64_t AsUInt(uint64_t def = 0) const;
    double AsReal(double def = 0) const;
    const std::string& AsString() const { return s_; }
    const std::vector<uint8_t>& AsData() const { return data_; }
    const std::vector<Plist>& Items() const { return arr_; }
    const std::vector<std::pair<std::string, Plist>>& Entries() const { return dict_; }

    const Plist* Get(const char* key) const;
    Plist& Set(const std::string& key, Plist v);
    Plist& Append(Plist v);

    // Human-readable single-line dump for logs (data abbreviated).
    std::string Describe(int depth = 0) const;

    static bool Parse(const uint8_t* p, size_t n, Plist& out);
    std::vector<uint8_t> Serialize() const;

private:
    Type type_ = Type::Null;
    uint64_t u_ = 0;
    double r_ = 0;
    std::string s_;
    std::vector<uint8_t> data_;
    std::vector<Plist> arr_;
    std::vector<std::pair<std::string, Plist>> dict_;

    friend class BplistReader;
    friend class BplistWriter;
};
