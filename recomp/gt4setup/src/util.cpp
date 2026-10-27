#include "util.h"

#include <charconv>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <system_error>

namespace gt4setup {

fs::path u8path(const std::string &s)
{
#if defined(__cpp_char8_t)
    return fs::path(std::u8string(s.begin(), s.end()));
#else
    return fs::u8path(s);
#endif
}

std::string pathstr(const fs::path &p)
{
#if defined(__cpp_char8_t)
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
#else
    return p.u8string();
#endif
}

Bytes readFile(const fs::path &p)
{
    std::ifstream f(p, std::ios::binary);
    if (!f)
        throw Error("cannot open " + pathstr(p));
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    f.seekg(0, std::ios::beg);
    Bytes b(static_cast<size_t>(n));
    if (n > 0 && !f.read(reinterpret_cast<char *>(b.data()), n))
        throw Error("cannot read " + pathstr(p));
    return b;
}

std::string readText(const fs::path &p)
{
    const Bytes b = readFile(p);
    return std::string(b.begin(), b.end());
}

void writeFile(const fs::path &p, const void *data, size_t size)
{
    if (p.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(p.parent_path(), ec);
    }
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f)
        throw Error("cannot create " + pathstr(p));
    if (size)
        f.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
    f.close();
    if (!f)
        throw Error("cannot write " + pathstr(p));
}

std::string strf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    const int n = std::vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    std::string s;
    if (n > 0) {
        s.resize(static_cast<size_t>(n) + 1);
        std::vsnprintf(&s[0], s.size(), fmt, ap2);
        s.resize(static_cast<size_t>(n));
    }
    va_end(ap2);
    return s;
}

std::string pyhex(uint64_t v)
{
    return strf("0x%llx", static_cast<unsigned long long>(v));
}

std::string toCrlf(const std::string &s)
{
    std::string o;
    o.reserve(s.size() + s.size() / 16);
    for (char c : s) {
        if (c == '\n')
            o += '\r';
        o += c;
    }
    return o;
}

uint16_t rd16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}
void wr16(uint8_t *p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
void wr32(uint8_t *p, uint32_t v)
{
    for (int k = 0; k < 4; ++k)
        p[k] = static_cast<uint8_t>(v >> (8 * k));
}

std::string pyFloatRepr(double v)
{
    if (std::isnan(v))
        return "nan";
    if (std::isinf(v))
        return v < 0 ? "-inf" : "inf";
    char buf[64];
    // shortest round-trip digits in scientific form: d[.ddd]e(+|-)XX
    const auto r = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::scientific);
    std::string sci(buf, r.ptr);
    bool neg = false;
    size_t i = 0;
    if (!sci.empty() && sci[0] == '-') {
        neg = true;
        i = 1;
    }
    const size_t e = sci.find('e');
    std::string digits;
    for (size_t k = i; k < e; ++k)
        if (sci[k] != '.')
            digits += sci[k];
    const int exp10 = std::atoi(sci.c_str() + e + 1);
    int decpt = exp10 + 1;
    // trailing zeros are never produced for the shortest form except "0"
    std::string out = neg ? "-" : "";
    const int nd = static_cast<int>(digits.size());
    if (decpt <= -4 || decpt > 16) {
        const int ex = decpt - 1;
        out += digits[0];
        if (nd > 1) {
            out += '.';
            out += digits.substr(1);
        }
        out += strf("e%c%02d", ex < 0 ? '-' : '+', ex < 0 ? -ex : ex);
        return out;
    }
    if (decpt <= 0) {
        out += "0.";
        out.append(static_cast<size_t>(-decpt), '0');
        out += digits;
    } else if (decpt < nd) {
        out += digits.substr(0, static_cast<size_t>(decpt));
        out += '.';
        out += digits.substr(static_cast<size_t>(decpt));
    } else {
        out += digits;
        out.append(static_cast<size_t>(decpt - nd), '0');
        out += ".0";
    }
    return out;
}

// ------------------------------------------------------------------ JSON
Json Json::boolean(bool b)
{
    Json j;
    j.type_ = Type::Bool;
    j.b_ = b;
    return j;
}
Json Json::integer(int64_t v)
{
    Json j;
    j.type_ = Type::Int;
    j.i_ = v;
    return j;
}
Json Json::string(const std::string &s)
{
    Json j;
    j.type_ = Type::String;
    j.s_ = s;
    return j;
}
Json Json::array()
{
    Json j;
    j.type_ = Type::Array;
    return j;
}
Json Json::object()
{
    Json j;
    j.type_ = Type::Object;
    return j;
}
Json &Json::push(Json v)
{
    arr_.push_back(std::move(v));
    return *this;
}
Json &Json::set(const std::string &k, Json v)
{
    for (auto &kv : obj_)
        if (kv.first == k) {
            kv.second = std::move(v);
            return *this;
        }
    obj_.emplace_back(k, std::move(v));
    return *this;
}
Json *Json::find(const std::string &k)
{
    for (auto &kv : obj_)
        if (kv.first == k)
            return &kv.second;
    return nullptr;
}

static void jsonString(std::string &out, const std::string &s)
{
    out += '"';
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = c;
        size_t len = 1;
        if (c >= 0x80) { // decode UTF-8 for \u escapes (ensure_ascii)
            if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
                cp = ((c & 0x1Fu) << 6) | (s[i + 1] & 0x3F);
                len = 2;
            } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
                cp = ((c & 0x0Fu) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F);
                len = 3;
            } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
                cp = ((c & 0x07u) << 18) | ((s[i + 1] & 0x3F) << 12) | ((s[i + 2] & 0x3F) << 6) | (s[i + 3] & 0x3F);
                len = 4;
            }
        }
        i += len;
        switch (cp) {
        case '"': out += "\\\""; continue;
        case '\\': out += "\\\\"; continue;
        case '\n': out += "\\n"; continue;
        case '\r': out += "\\r"; continue;
        case '\t': out += "\\t"; continue;
        case '\b': out += "\\b"; continue;
        case '\f': out += "\\f"; continue;
        default: break;
        }
        if (cp < 0x20 || (cp >= 0x7F && cp < 0x10000 && cp != 0x7F)) {
            out += strf("\\u%04x", cp);
        } else if (cp == 0x7F) {
            out += static_cast<char>(0x7F);
        } else if (cp >= 0x10000) {
            const uint32_t v = cp - 0x10000;
            out += strf("\\u%04x\\u%04x", 0xD800 + (v >> 10), 0xDC00 + (v & 0x3FF));
        } else {
            out += static_cast<char>(cp);
        }
    }
    out += '"';
}

void Json::dumpTo(std::string &out, int indent, int level) const
{
    switch (type_) {
    case Type::Null: out += "null"; return;
    case Type::Bool: out += b_ ? "true" : "false"; return;
    case Type::Int: out += std::to_string(i_); return;
    case Type::String: jsonString(out, s_); return;
    case Type::Array:
    case Type::Object: {
        const bool isArr = type_ == Type::Array;
        const size_t n = isArr ? arr_.size() : obj_.size();
        if (n == 0) {
            out += isArr ? "[]" : "{}";
            return;
        }
        out += isArr ? '[' : '{';
        for (size_t k = 0; k < n; ++k) {
            if (indent >= 0) {
                out += k ? ",\n" : "\n";
                out.append(static_cast<size_t>(indent * (level + 1)), ' ');
            } else if (k) {
                out += ", ";
            }
            if (isArr) {
                arr_[k].dumpTo(out, indent, level + 1);
            } else {
                jsonString(out, obj_[k].first);
                out += ": ";
                obj_[k].second.dumpTo(out, indent, level + 1);
            }
        }
        if (indent >= 0) {
            out += '\n';
            out.append(static_cast<size_t>(indent * level), ' ');
        }
        out += isArr ? ']' : '}';
        return;
    }
    }
}

std::string Json::dump(int indent) const
{
    std::string s;
    dumpTo(s, indent, 0);
    return s;
}

} // namespace gt4setup
