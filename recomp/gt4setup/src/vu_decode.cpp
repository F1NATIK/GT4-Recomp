#include "vu_decode.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace gt4setup {
namespace vu {

void Usage::addVfRead(int reg, int lanes)
{
    if (lanes == 0)
        return;
    for (int k = 0; k < vfReadCount; ++k)
        if (vfReadReg[k] == reg) {
            vfReadLanes[k] |= lanes;
            return;
        }
    if (vfReadCount < 2) {
        vfReadReg[vfReadCount] = reg;
        vfReadLanes[vfReadCount] = lanes;
        ++vfReadCount;
    }
}

void Usage::addVfWrite(int reg, int lanes)
{
    if (reg == 0 || lanes == 0)
        return;
    if (vfWriteReg == 0) {
        vfWriteReg = reg;
        vfWriteLanes = lanes;
    } else if (vfWriteReg == reg) {
        vfWriteLanes |= lanes;
    }
}

int Usage::vfReadLanesOf(int reg) const
{
    for (int k = 0; k < vfReadCount; ++k)
        if (vfReadReg[k] == reg)
            return vfReadLanes[k];
    return 0;
}

static bool in(int v, std::initializer_list<int> set)
{
    for (int x : set)
        if (x == v)
            return true;
    return false;
}

Usage upperUsage(uint32_t upper)
{
    Usage u;
    u.pipeline = PIPE_FMAC;
    u.latency = FMAC_LATENCY;
    const int op = upper & 0x3F;
    const int dest = DEST(upper), fs = FS(upper), ft = FT(upper), fd = FD(upper);
    if (op <= 0x2F) {
        u.addVfRead(fs, dest);
        u.addVfWrite(fd, dest);
        if (op <= 0x1B)
            u.addVfRead(ft, lane(op & 3));
        else if (op >= 0x28)
            u.addVfRead(ft, op == 0x2E ? 0xE : dest);
        if (in(op, {0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x21, 0x23, 0x25, 0x27, 0x29, 0x2D, 0x2E}))
            u.accRead = dest;
        return u;
    }
    if (op >= 0x3C) {
        const int sp = (upper & 3) | ((upper >> 4) & 0x7C);
        const bool writesAcc = sp <= 0x0F || (0x18 <= sp && sp <= 0x1C) || sp == 0x1E || (0x20 <= sp && sp <= 0x2A) ||
                               (0x2C <= sp && sp <= 0x2E);
        if (writesAcc) {
            u.addVfRead(fs, dest);
            if (sp <= 0x1B)
                u.addVfRead(ft, lane(sp & 3));
            else if (0x28 <= sp && sp <= 0x2E)
                u.addVfRead(ft, sp == 0x2E ? 0xE : dest);
            u.accWrite = dest;
            if ((0x08 <= sp && sp <= 0x0F) || in(sp, {0x21, 0x23, 0x25, 0x27, 0x29, 0x2D}))
                u.accRead = dest;
        } else if (0x10 <= sp && sp <= 0x17) {
            u.addVfRead(fs, dest);
            u.addVfWrite(ft, dest);
        } else if (sp == 0x1D) {
            u.addVfRead(fs, dest);
            u.addVfWrite(ft, dest);
        } else if (sp == 0x1F) {
            u.addVfRead(fs, 0xE);
            u.addVfRead(ft, 0x1);
            u.writesClip = true;
        } else if (sp != 0x2F && sp != 0x30) {
            u.reserved = true;
        }
        return u;
    }
    u.reserved = true;
    return u;
}

Usage lowerUsage(uint32_t lower)
{
    Usage u;
    if (lower == 0 || lower == 0x8000033C)
        return u;
    const int op = (lower >> 25) & 0x7F;
    const int vfT = FT(lower), vfS = FS(lower), viT = VIT(lower), viS = VIS(lower), viD = VID(lower), dest = DEST(lower);
    auto rvi = [&](int r) {
        if (r)
            u.viRead |= 1u << r;
    };
    auto wvi = [&](int r) {
        if (r)
            u.viWrite |= 1u << r;
    };
    auto set = [&](int pipe, int lat) {
        u.pipeline = pipe;
        u.latency = lat;
    };
    switch (op) {
    case 0x00: set(PIPE_LSU, 4); rvi(viS); u.addVfWrite(vfT, dest); return u;
    case 0x01: set(PIPE_LSU, 1); rvi(viT); u.addVfRead(vfS, dest); return u;
    case 0x04: set(PIPE_LSU, 4); rvi(viS); wvi(viT); return u;
    case 0x05: set(PIPE_LSU, 1); rvi(viS); rvi(viT); return u;
    case 0x08: case 0x09: set(PIPE_IALU, 1); u.delaysNextBranchRead = true; rvi(viS); wvi(viT); return u;
    case 0x10: case 0x12: case 0x13: set(PIPE_IALU, 1); u.readsClip = true; wvi(1); return u;
    case 0x11: set(PIPE_FMAC, FMAC_LATENCY); u.writesClip = true; return u;
    case 0x14: case 0x16: case 0x17: set(PIPE_IALU, 1); wvi(viT); return u;
    case 0x15: set(PIPE_FMAC, FMAC_LATENCY); return u;
    case 0x18: case 0x1A: case 0x1B: set(PIPE_IALU, 1); rvi(viS); wvi(viT); return u;
    case 0x1C: set(PIPE_IALU, 1); u.readsClip = true; wvi(viT); return u;
    case 0x20: u.pipeline = PIPE_BRANCH; return u;
    case 0x21: set(PIPE_BRANCH, 1); wvi(viT); return u;
    case 0x24: u.pipeline = PIPE_BRANCH; rvi(viS); return u;
    case 0x25: set(PIPE_BRANCH, 1); rvi(viS); wvi(viT); return u;
    case 0x28: case 0x29: u.pipeline = PIPE_BRANCH; rvi(viS); rvi(viT); return u;
    case 0x2C: case 0x2D: case 0x2E: case 0x2F: u.pipeline = PIPE_BRANCH; rvi(viS); return u;
    default: break;
    }
    if (op != 0x40) {
        u.reserved = true;
        return u;
    }
    const int direct = lower & 0x3F;
    if (in(direct, {0x30, 0x31, 0x34, 0x35})) {
        set(PIPE_IALU, 1); u.delaysNextBranchRead = true; rvi(viS); rvi(viT); wvi(viD); return u;
    }
    if (direct == 0x32) {
        set(PIPE_IALU, 1); u.delaysNextBranchRead = true; rvi(viS); wvi(viT); return u;
    }
    if (direct < 0x3C) {
        u.reserved = true;
        return u;
    }
    const int sp = (lower & 3) | ((lower >> 4) & 0x7C);
    if (sp == 0x30 || sp == 0x31) {
        set(PIPE_FMAC, 4); u.addVfRead(vfS, sp == 0x31 ? 0xF : dest); u.addVfWrite(vfT, dest);
    } else if (sp == 0x34 || sp == 0x36) {
        set(PIPE_LSU, 4); u.viLatency = 1; u.delaysNextBranchRead = true;
        rvi(viS); wvi(viS); u.addVfWrite(vfT, dest);
    } else if (sp == 0x35 || sp == 0x37) {
        set(PIPE_LSU, 1); u.delaysNextBranchRead = true;
        rvi(viT); wvi(viT); u.addVfRead(vfS, dest);
    } else if (sp == 0x38) {
        set(PIPE_FDIV, 7);
        u.addVfRead(vfS, lane((lower >> 21) & 3)); u.addVfRead(vfT, lane((lower >> 23) & 3));
    } else if (sp == 0x39) {
        set(PIPE_FDIV, 7); u.addVfRead(vfT, lane((lower >> 23) & 3));
    } else if (sp == 0x3A) {
        set(PIPE_FDIV, 13);
        u.addVfRead(vfS, lane((lower >> 21) & 3)); u.addVfRead(vfT, lane((lower >> 23) & 3));
    } else if (sp == 0x3B) {
        u.pipeline = PIPE_FDIV; u.waitQ = true;
    } else if (sp == 0x3C) {
        set(PIPE_IALU, 1); u.delaysNextBranchRead = true;
        u.addVfRead(vfS, lane((lower >> 21) & 3)); wvi(viT);
    } else if (sp == 0x3D) {
        set(PIPE_FMAC, 4); rvi(viS); u.addVfWrite(vfT, dest);
    } else if (sp == 0x3E) {
        set(PIPE_LSU, 4); rvi(viS); wvi(viT);
    } else if (sp == 0x3F) {
        set(PIPE_LSU, 1); rvi(viS); rvi(viT);
    } else if (sp == 0x40 || sp == 0x41) {
        set(PIPE_FMAC, 4); u.addVfWrite(vfT, dest);
    } else if (sp == 0x42 || sp == 0x43) {
        set(PIPE_IALU, 1); u.addVfRead(vfS, lane((lower >> 21) & 3));
    } else if (sp == 0x64) {
        set(PIPE_FMAC, 4); u.addVfWrite(vfT, dest);
    } else if (sp == 0x68 || sp == 0x69) {
        set(PIPE_IALU, 1); wvi(viT);
    } else if (sp == 0x6C) {
        set(PIPE_XGKICK, 2); rvi(viS);
    } else if (in(sp, {0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7C, 0x7D})) {
        u.pipeline = PIPE_EFU;
        switch (sp) {
        case 0x70: u.latency = 11; break;
        case 0x71: case 0x72: case 0x77: u.latency = 18; break;
        case 0x73: u.latency = 24; break;
        case 0x74: case 0x75: case 0x7C: u.latency = 54; break;
        case 0x76: case 0x78: case 0x7A: u.latency = 12; break;
        case 0x79: u.latency = 29; break;
        case 0x7D: u.latency = 44; break;
        default: break;
        }
        if (0x70 <= sp && sp <= 0x73)
            u.addVfRead(vfS, 0xE);
        else if (sp == 0x74)
            u.addVfRead(vfS, 0xC);
        else if (sp == 0x75)
            u.addVfRead(vfS, 0xA);
        else if (sp == 0x76)
            u.addVfRead(vfS, 0xF);
        else
            u.addVfRead(vfS, lane((lower >> 21) & 3));
    } else if (sp == 0x7B) {
        u.pipeline = PIPE_EFU; u.waitP = true;
    } else {
        u.reserved = true;
    }
    return u;
}

Pair decodePair(const uint8_t *code, uint32_t pc)
{
    Pair p;
    p.pc = pc;
    p.lower = rd32(code + pc);
    p.upper = rd32(code + pc + 4);
    p.uu = upperUsage(p.upper);
    p.iBit = (p.upper & 0x80000000u) != 0;
    p.lu = p.iBit ? Usage() : lowerUsage(p.lower);
    p.eBit = (p.upper & 0x40000000u) != 0;
    p.mBit = (p.upper & 0x20000000u) != 0;
    p.dBit = (p.upper & 0x10000000u) != 0;
    p.tBit = (p.upper & 0x08000000u) != 0;
    const int w = p.uu.vfWriteReg;
    if (w && (p.lu.vfReadLanesOf(w) || p.lu.vfWriteReg == w)) {
        p.upperShadow = w;
        if (p.lu.vfWriteReg == w)
            p.suppressedLowerVf = w;
    }
    return p;
}

// ---------------------------------------------------------------- disassembly
namespace {

const char *const BC = "xyzw";

std::string destStr(int d)
{
    std::string s;
    for (int k = 0; k < 4; ++k)
        if (d & lane(k))
            s += BC[k];
    return s;
}

struct Tables {
    std::map<int, std::string> upperNames, upperSpecial, lowerSpecial, lowerNames;
    Tables()
    {
        const char *kinds[] = {"add", "sub", "madd", "msub", "max", "mini", "mul"};
        for (int k = 0; k < 7; ++k)
            for (int b = 0; b < 4; ++b)
                upperNames[k * 4 + b] = std::string(kinds[k]) + BC[b];
        const std::pair<int, const char *> un[] = {
            {0x1C, "mulq"}, {0x1D, "maxi"}, {0x1E, "muli"}, {0x1F, "minii"}, {0x20, "addq"}, {0x21, "maddq"},
            {0x22, "addi"}, {0x23, "maddi"}, {0x24, "subq"}, {0x25, "msubq"}, {0x26, "subi"}, {0x27, "msubi"},
            {0x28, "add"}, {0x29, "madd"}, {0x2A, "mul"}, {0x2B, "max"}, {0x2C, "sub"}, {0x2D, "msub"},
            {0x2E, "opmsub"}, {0x2F, "mini"}};
        for (const auto &e : un)
            upperNames[e.first] = e.second;
        const char *acc[] = {"adda", "suba", "madda", "msuba"};
        for (int k = 0; k < 4; ++k)
            for (int b = 0; b < 4; ++b)
                upperSpecial[k * 4 + b] = std::string(acc[k]) + BC[b];
        for (int b = 0; b < 4; ++b)
            upperSpecial[0x18 + b] = std::string("mula") + BC[b];
        const std::pair<int, const char *> us[] = {
            {0x10, "itof0"}, {0x11, "itof4"}, {0x12, "itof12"}, {0x13, "itof15"}, {0x14, "ftoi0"}, {0x15, "ftoi4"},
            {0x16, "ftoi12"}, {0x17, "ftoi15"}, {0x1C, "mulaq"}, {0x1D, "abs"}, {0x1E, "mulai"}, {0x1F, "clip"},
            {0x20, "addaq"}, {0x21, "maddaq"}, {0x22, "addai"}, {0x23, "maddai"}, {0x24, "subaq"}, {0x25, "msubaq"},
            {0x26, "subai"}, {0x27, "msubai"}, {0x28, "adda"}, {0x29, "madda"}, {0x2A, "mula"}, {0x2C, "suba"},
            {0x2D, "msuba"}, {0x2E, "opmula"}, {0x2F, "nop"}, {0x30, "nop"}};
        for (const auto &e : us)
            upperSpecial[e.first] = e.second;
        const std::pair<int, const char *> ls[] = {
            {0x30, "move"}, {0x31, "mr32"}, {0x34, "lqi"}, {0x35, "sqi"}, {0x36, "lqd"}, {0x37, "sqd"},
            {0x38, "div"}, {0x39, "sqrt"}, {0x3A, "rsqrt"}, {0x3B, "waitq"}, {0x3C, "mtir"}, {0x3D, "mfir"},
            {0x3E, "ilwr"}, {0x3F, "iswr"}, {0x40, "rnext"}, {0x41, "rget"}, {0x42, "rinit"}, {0x43, "rxor"},
            {0x64, "mfp"}, {0x68, "xtop"}, {0x69, "xitop"}, {0x6C, "xgkick"}, {0x70, "esadd"}, {0x71, "ersadd"},
            {0x72, "eleng"}, {0x73, "erleng"}, {0x74, "eatanxy"}, {0x75, "eatanxz"}, {0x76, "esum"},
            {0x77, "ersqrt"}, {0x78, "esqrt"}, {0x79, "esin"}, {0x7A, "ercpr"}, {0x7B, "waitp"}, {0x7C, "eatan"},
            {0x7D, "eexp"}};
        for (const auto &e : ls)
            lowerSpecial[e.first] = e.second;
        const std::pair<int, const char *> ln[] = {
            {0x00, "lq"}, {0x01, "sq"}, {0x04, "ilw"}, {0x05, "isw"}, {0x08, "iaddiu"}, {0x09, "isubiu"},
            {0x10, "fceq"}, {0x11, "fcset"}, {0x12, "fcand"}, {0x13, "fcor"}, {0x14, "fseq"}, {0x15, "fsset"},
            {0x16, "fsand"}, {0x17, "fsor"}, {0x18, "fmeq"}, {0x1A, "fmand"}, {0x1B, "fmor"}, {0x1C, "fcget"},
            {0x20, "b"}, {0x21, "bal"}, {0x24, "jr"}, {0x25, "jalr"}, {0x28, "ibeq"}, {0x29, "ibne"},
            {0x2C, "ibltz"}, {0x2D, "ibgtz"}, {0x2E, "iblez"}, {0x2F, "ibgez"}};
        for (const auto &e : ln)
            lowerNames[e.first] = e.second;
    }
};

const Tables &tables()
{
    static const Tables t;
    return t;
}

std::string get(const std::map<int, std::string> &m, int k, const char *def)
{
    const auto it = m.find(k);
    return it == m.end() ? std::string(def) : it->second;
}

bool oneOf(const std::string &n, std::initializer_list<const char *> set)
{
    for (const char *x : set)
        if (n == x)
            return true;
    return false;
}

std::string fmtUpper(uint32_t u)
{
    const std::string n = upperName(u);
    const std::string d = destStr(DEST(u));
    const int op = u & 0x3F;
    if (n == "nop")
        return "nop";
    if (op < 0x3C) {
        if (op <= 0x1B)
            return strf("%s.%s vf%d, vf%d, vf%d%c", n.c_str(), d.c_str(), FD(u), FS(u), FT(u), BC[op & 3]);
        if (op >= 0x1C && op <= 0x27)
            return strf("%s.%s vf%d, vf%d", n.c_str(), d.c_str(), FD(u), FS(u));
        return strf("%s.%s vf%d, vf%d, vf%d", n.c_str(), d.c_str(), FD(u), FS(u), FT(u));
    }
    const int sp = (u & 3) | ((u >> 4) & 0x7C);
    if ((0x10 <= sp && sp <= 0x17) || sp == 0x1D)
        return strf("%s.%s vf%d, vf%d", n.c_str(), d.c_str(), FT(u), FS(u));
    if (sp == 0x1F)
        return strf("clip vf%d.xyz, vf%d.w", FS(u), FT(u));
    if (sp <= 0x0F || (0x18 <= sp && sp <= 0x1B))
        return strf("%s.%s acc, vf%d, vf%d%c", n.c_str(), d.c_str(), FS(u), FT(u), BC[sp & 3]);
    return strf("%s.%s acc, vf%d, vf%d", n.c_str(), d.c_str(), FS(u), FT(u));
}

std::string fmtLower(uint32_t l, uint32_t pc)
{
    const std::string n = lowerName(l);
    if (n == "nop")
        return "nop";
    const std::string d = destStr(DEST(l));
    const char *nc = n.c_str();
    const char *dc = d.c_str();
    if (oneOf(n, {"lq", "sq", "ilw", "isw"})) {
        int base = (n == "lq" || n == "ilw") ? VIS(l) : VIT(l);
        base = n == "isw" ? VIS(l) : base;
        const std::string reg = n == "lq" ? strf("vf%d", FT(l)) : n == "sq" ? strf("vf%d", FS(l)) : strf("vi%d", VIT(l));
        return strf("%s.%s %s, %d(vi%d)", nc, dc, reg.c_str(), IMM11(l), base);
    }
    if (oneOf(n, {"b", "bal", "ibeq", "ibne", "ibltz", "ibgtz", "iblez", "ibgez"})) {
        const uint32_t tgt = static_cast<uint32_t>(static_cast<int>(pc) + 8 + IMM11(l) * 8) & 0x3FFF;
        if (n == "b")
            return strf("b 0x%x", tgt);
        if (n == "bal")
            return strf("bal vi%d, 0x%x", VIT(l), tgt);
        if (n == "ibeq" || n == "ibne")
            return strf("%s vi%d, vi%d, 0x%x", nc, VIT(l), VIS(l), tgt);
        return strf("%s vi%d, 0x%x", nc, VIS(l), tgt);
    }
    if (n == "jr")
        return strf("jr vi%d", VIS(l));
    if (n == "jalr")
        return strf("jalr vi%d, vi%d", VIT(l), VIS(l));
    if (n == "iaddiu" || n == "isubiu") {
        const uint32_t imm = (l & 0x7FF) | ((l >> 10) & 0x7800);
        return strf("%s vi%d, vi%d, 0x%x", nc, VIT(l), VIS(l), imm);
    }
    if (oneOf(n, {"iadd", "isub", "iand", "ior"}))
        return strf("%s vi%d, vi%d, vi%d", nc, VID(l), VIS(l), VIT(l));
    if (n == "iaddi") {
        int imm = (l >> 6) & 0x1F;
        imm = (imm & 16) ? imm - 32 : imm;
        return strf("iaddi vi%d, vi%d, %d", VIT(l), VIS(l), imm);
    }
    if (n == "lqi" || n == "lqd")
        return strf("%s.%s vf%d, (vi%d%s)", nc, dc, FT(l), VIS(l), n == "lqi" ? "++" : "--");
    if (n == "sqi" || n == "sqd")
        return strf("%s.%s vf%d, (vi%d%s)", nc, dc, FS(l), VIT(l), n == "sqi" ? "++" : "--");
    if (n == "move" || n == "mr32")
        return strf("%s.%s vf%d, vf%d", nc, dc, FT(l), FS(l));
    if (n == "div" || n == "rsqrt")
        return strf("%s q, vf%d%c, vf%d%c", nc, FS(l), BC[(l >> 21) & 3], FT(l), BC[(l >> 23) & 3]);
    if (n == "sqrt")
        return strf("sqrt q, vf%d%c", FT(l), BC[(l >> 23) & 3]);
    if (n == "mtir")
        return strf("mtir vi%d, vf%d%c", VIT(l), FS(l), BC[(l >> 21) & 3]);
    if (n == "mfir")
        return strf("mfir.%s vf%d, vi%d", dc, FT(l), VIS(l));
    if (n == "ilwr" || n == "iswr")
        return strf("%s.%s vi%d, (vi%d)", nc, dc, VIT(l), VIS(l));
    if (oneOf(n, {"mfp", "rnext", "rget"}))
        return strf("%s.%s vf%d", nc, dc, FT(l));
    if (n == "xtop" || n == "xitop")
        return strf("%s vi%d", nc, VIT(l));
    if (n == "xgkick")
        return strf("xgkick vi%d", VIS(l));
    if (oneOf(n, {"fmeq", "fmand", "fmor"}))
        return strf("%s vi%d, vi%d", nc, VIT(l), VIS(l));
    if (oneOf(n, {"fsand", "fsor", "fseq", "fsset"}))
        return strf("%s vi%d, 0x%x", nc, VIT(l), (((l >> 21) & 1u) << 11) | (l & 0x7FFu));
    if (oneOf(n, {"fcand", "fcor", "fceq", "fcset"}))
        return strf("%s vi1, 0x%x", nc, l & 0xFFFFFFu);
    if (n == "fcget")
        return strf("fcget vi%d", VIT(l));
    if ((!n.empty() && n[0] == 'e') || n == "rinit" || n == "rxor")
        return strf("%s p, vf%d", nc, FS(l));
    return n;
}

std::string padRight(const std::string &s, size_t w)
{
    return s.size() >= w ? s : s + std::string(w - s.size(), ' ');
}

} // namespace

std::string upperName(uint32_t upper)
{
    const int op = upper & 0x3F;
    if (op >= 0x3C)
        return get(tables().upperSpecial, (upper & 3) | ((upper >> 4) & 0x7C), "u?");
    return get(tables().upperNames, op, "u?");
}

std::string lowerName(uint32_t lower)
{
    if (lower == 0 || lower == 0x8000033C)
        return "nop";
    const int op = (lower >> 25) & 0x7F;
    if (op == 0x40) {
        const int d = lower & 0x3F;
        if (d < 0x3C) {
            switch (d) {
            case 0x30: return "iadd";
            case 0x31: return "isub";
            case 0x32: return "iaddi";
            case 0x34: return "iand";
            case 0x35: return "ior";
            default: return "l?";
            }
        }
        return get(tables().lowerSpecial, (lower & 3) | ((lower >> 4) & 0x7C), "l?");
    }
    return get(tables().lowerNames, op, "l?");
}

std::string fmtPair(const Pair &p)
{
    std::string flags;
    const bool bits[5] = {p.iBit, p.eBit, p.mBit, p.dBit, p.tBit};
    for (int k = 0; k < 5; ++k)
        if (bits[k])
            flags += "IEMDT"[k];
    std::string lower;
    if (p.iBit) {
        float f;
        std::memcpy(&f, &p.lower, 4);
        lower = "loi " + pyFloatRepr(static_cast<double>(f));
    } else {
        lower = fmtLower(p.lower, p.pc);
    }
    return strf("%04x: ", p.pc) + padRight(flags, 5) + " " + padRight(fmtUpper(p.upper), 34) + " " + lower;
}

Bytes loadCode(const Bytes &d)
{
    static const char magic[] = "GT3VU1K1";
    if (d.size() >= 8 && std::memcmp(d.data(), magic, 8) == 0) {
        const size_t a = 8 + 32 + 8 + 664;
        const size_t b = std::min(d.size(), a + 16384);
        return a < d.size() ? Bytes(d.begin() + static_cast<std::ptrdiff_t>(a), d.begin() + static_cast<std::ptrdiff_t>(b))
                            : Bytes();
    }
    return Bytes(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(std::min<size_t>(d.size(), 16384)));
}

} // namespace vu
} // namespace gt4setup
