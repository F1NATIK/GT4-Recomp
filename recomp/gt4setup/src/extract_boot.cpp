// extract-boot: port of tools/extract_boot_files.py.
// Copies GT4's boot files (everything outside GT4.VOL and the EPSON printer
// drivers) from the ISO into <outdir>, reading the ISO only, and writes
// <outdir>/boot-files-manifest.json (SHA-256 of every file) exactly as the
// Python script wrote it on Windows (json indent 1, CRLF line ends).
#include "util.h"

#include <cstdio>
#include <fstream>
#include <iostream>

namespace gt4setup {

namespace {

struct Iso {
    std::ifstream f;
    Bytes read(uint64_t offset, size_t size)
    {
        Bytes b(size);
        f.clear();
        f.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        f.read(reinterpret_cast<char *>(b.data()), static_cast<std::streamsize>(size));
        b.resize(static_cast<size_t>(f.gcount())); // short read at the end of the file, as Python's read()
        return b;
    }
    Bytes sec(uint32_t n, uint32_t c = 1) { return read(static_cast<uint64_t>(n) * 2048u, 2048u * c); }
};

struct Entry {
    std::string name;
    uint32_t lba, size;
    bool isdir;
};

std::vector<Entry> entries(Iso &iso, uint32_t lba, uint32_t size)
{
    std::vector<Entry> out;
    const Bytes data = iso.sec(lba, (size + 2047) / 2048);
    size_t i = 0;
    while (i < size) {
        if (i >= data.size())
            throw Error("directory extent runs past the end of the ISO");
        const uint8_t n = data[i];
        if (n == 0) {
            i = (i / 2048 + 1) * 2048;
            continue;
        }
        if (i + n > data.size() || n < 34)
            throw Error("bad directory record");
        const uint8_t *rec = &data[i];
        i += n;
        const uint8_t nl = rec[32];
        if (33u + nl > n)
            throw Error("bad directory record name");
        std::string name(reinterpret_cast<const char *>(rec + 33), nl);
        if (name == std::string(1, '\0') || name == std::string(1, '\1'))
            continue;
        const size_t semi = name.find(';');
        if (semi != std::string::npos)
            name.resize(semi);
        out.push_back({name, rd32(rec + 2), rd32(rec + 10), (rec[25] & 2) != 0});
    }
    return out;
}

bool pySpace(unsigned char c) { return c == ' ' || (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x1F); }

std::string pyStrip(const std::string &s)
{
    size_t a = 0, b = s.size();
    while (a < b && pySpace(static_cast<unsigned char>(s[a])))
        ++a;
    while (b > a && pySpace(static_cast<unsigned char>(s[b - 1])))
        --b;
    return s.substr(a, b - a);
}

void walk(Iso &iso, const fs::path &out, Json &manifest, uint32_t lba, uint32_t size, const std::string &prefix)
{
    for (const Entry &e : entries(iso, lba, size)) {
        const std::string rel = prefix + e.name;
        if (e.isdir) {
            if (e.name != "EPSON")
                walk(iso, out, manifest, e.lba, e.size, rel + "/");
            continue;
        }
        if (e.name == "GT4.VOL") {
            Json m = Json::object();
            m.set("lba", Json::integer(e.lba));
            m.set("size", Json::integer(e.size));
            m.set("extracted", Json::boolean(false));
            manifest.set(rel, m);
            continue;
        }
        const Bytes data = iso.read(static_cast<uint64_t>(e.lba) * 2048u, e.size);
        writeFile(out / u8path(rel), data);
        Json m = Json::object();
        m.set("lba", Json::integer(e.lba));
        m.set("size", Json::integer(e.size));
        m.set("sha256", Json::string(sha256Hex(data)));
        manifest.set(rel, m);
    }
}

} // namespace

int cmdExtractBoot(const Args &args)
{
    if (args.size() != 2) {
        std::fprintf(stderr, "usage: gt4setup extract-boot <iso> <outdir>\n");
        return 2;
    }
    const fs::path isoPath = u8path(args[0]), out = u8path(args[1]);
    Iso iso;
    iso.f.open(isoPath, std::ios::binary);
    if (!iso.f)
        throw Error("cannot open " + args[0]);
    const Bytes pvd = iso.sec(16);
    if (pvd.size() < 2048)
        throw Error("not an ISO (no volume descriptor)");
    Json manifest = Json::object();
    walk(iso, out, manifest, rd32(&pvd[156 + 2]), rd32(&pvd[156 + 10]), "");
    Json vol = Json::object();
    vol.set("id", Json::string(pyStrip(std::string(reinterpret_cast<const char *>(&pvd[40]), 32))));
    vol.set("iso9660_blocks", Json::integer(rd32(&pvd[80])));
    vol.set("iso_bytes", Json::integer(static_cast<int64_t>(fs::file_size(isoPath))));
    const std::string volLine = vol.dump();
    manifest.set("_volume", vol);
    writeFile(out / "boot-files-manifest.json", toCrlf(manifest.dump(1) + "\n"));
    std::printf("%s %d files\n", volLine.c_str(), static_cast<int>(manifest.size()) - 1);
    return 0;
}

} // namespace gt4setup
