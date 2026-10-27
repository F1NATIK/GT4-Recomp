// Shared helpers for gt4setup: files, formatting, hashes, Python-compatible JSON.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace gt4setup {

namespace fs = std::filesystem;

using Bytes = std::vector<uint8_t>;
using Args = std::vector<std::string>; // UTF-8 command-line arguments after the subcommand

struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// UTF-8 string <-> path (portable across MSVC and mingw).
fs::path u8path(const std::string &s);
std::string pathstr(const fs::path &p);

Bytes readFile(const fs::path &p);
std::string readText(const fs::path &p); // raw bytes as a string
void writeFile(const fs::path &p, const void *data, size_t size);
inline void writeFile(const fs::path &p, const Bytes &b) { writeFile(p, b.data(), b.size()); }
inline void writeFile(const fs::path &p, const std::string &s) { writeFile(p, s.data(), s.size()); }

// printf-style formatting into a std::string.
std::string strf(const char *fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

// Python hex(): "0x" + lowercase digits, no padding.
std::string pyhex(uint64_t v);

// "\n" -> "\r\n" (what Python's text-mode writes produce on Windows).
std::string toCrlf(const std::string &s);

uint16_t rd16(const uint8_t *p);
uint32_t rd32(const uint8_t *p);
void wr16(uint8_t *p, uint16_t v);
void wr32(uint8_t *p, uint32_t v);

// SHA-256 (sha256.cpp)
std::string sha256Hex(const void *data, size_t size);
inline std::string sha256Hex(const Bytes &b) { return sha256Hex(b.data(), b.size()); }

// Python float repr() of a double (shortest round-trip digits, Python's layout).
std::string pyFloatRepr(double v);

// Minimal JSON value with Python json.dumps formatting (ensure_ascii, insertion order).
class Json {
public:
    enum class Type { Null, Bool, Int, String, Array, Object };
    Json() = default;
    static Json boolean(bool b);
    static Json integer(int64_t v);
    static Json string(const std::string &s);
    static Json array();
    static Json object();

    Json &push(Json v);                         // array append
    Json &set(const std::string &k, Json v);    // object set (keeps the first position of a key)
    Json *find(const std::string &k);
    size_t size() const { return type_ == Type::Array ? arr_.size() : obj_.size(); }

    // indent < 0: compact Python default (", " and ": "); else json.dumps(indent=N).
    std::string dump(int indent = -1) const;

private:
    void dumpTo(std::string &out, int indent, int level) const;
    Type type_ = Type::Null;
    bool b_ = false;
    int64_t i_ = 0;
    std::string s_;
    std::vector<Json> arr_;
    std::vector<std::pair<std::string, Json>> obj_;
};

// Subcommands
int cmdExtractBoot(const Args &args);
int cmdCoreToElf(const Args &args);
int cmdVuImages(const Args &args);
int cmdVuTranslate(const Args &args);
int cmdVuDisasm(const Args &args);
int cmdLateFunctions(const Args &args);
int cmdBuild(const Args &args);

} // namespace gt4setup
