#pragma once

#include <cstddef>
#include <array>
#include <cstdint>
#include <vector>

enum GSPrimType : uint8_t
{
    GS_PRIM_POINT = 0,
    GS_PRIM_LINE = 1,
    GS_PRIM_LINESTRIP = 2,
    GS_PRIM_TRIANGLE = 3,
    GS_PRIM_TRISTRIP = 4,
    GS_PRIM_TRIFAN = 5,
    GS_PRIM_SPRITE = 6,
};

enum GSPsm : uint8_t
{
    GS_PSM_CT32 = 0,
    GS_PSM_CT24 = 1,
    GS_PSM_CT16 = 2,
    GS_PSM_CT16S = 10,
    GS_PSM_T8 = 19,
    GS_PSM_T4 = 20,
    GS_PSM_T8H = 27,
    GS_PSM_T4HL = 36,
    GS_PSM_T4HH = 44,
    GS_PSM_Z32 = 48,
    GS_PSM_Z24 = 49,
    GS_PSM_Z16 = 50,
    GS_PSM_Z16S = 58,
};

enum GSGifFormat : uint8_t
{
    GIF_FMT_PACKED = 0,
    GIF_FMT_REGLIST = 1,
    GIF_FMT_IMAGE = 2,
    GIF_FMT_DISABLED = 3,
};

enum GSRegId : uint8_t
{
    GS_REG_PRIM = 0x00,
    GS_REG_RGBAQ = 0x01,
    GS_REG_ST = 0x02,
    GS_REG_UV = 0x03,
    GS_REG_XYZF2 = 0x04,
    GS_REG_XYZ2 = 0x05,
    GS_REG_TEX0_1 = 0x06,
    GS_REG_TEX0_2 = 0x07,
    GS_REG_CLAMP_1 = 0x08,
    GS_REG_CLAMP_2 = 0x09,
    GS_REG_FOG = 0x0A,
    GS_REG_XYZF3 = 0x0C,
    GS_REG_XYZ3 = 0x0D,
    GS_REG_AD = 0x0F,
    GS_REG_TEX1_1 = 0x14,
    GS_REG_TEX1_2 = 0x15,
    GS_REG_TEX2_1 = 0x16,
    GS_REG_TEX2_2 = 0x17,
    GS_REG_XYOFFSET_1 = 0x18,
    GS_REG_XYOFFSET_2 = 0x19,
    GS_REG_PRMODECONT = 0x1A,
    GS_REG_PRMODE = 0x1B,
    GS_REG_TEXCLUT = 0x1C,
    GS_REG_SCANMSK = 0x22,
    GS_REG_MIPTBP1_1 = 0x34,
    GS_REG_MIPTBP1_2 = 0x35,
    GS_REG_MIPTBP2_1 = 0x36,
    GS_REG_MIPTBP2_2 = 0x37,
    GS_REG_TEXA = 0x3B,
    GS_REG_FOGCOL = 0x3D,
    GS_REG_TEXFLUSH = 0x3F,
    GS_REG_SCISSOR_1 = 0x40,
    GS_REG_SCISSOR_2 = 0x41,
    GS_REG_ALPHA_1 = 0x42,
    GS_REG_ALPHA_2 = 0x43,
    GS_REG_DIMX = 0x44,
    GS_REG_DTHE = 0x45,
    GS_REG_COLCLAMP = 0x46,
    GS_REG_TEST_1 = 0x47,
    GS_REG_TEST_2 = 0x48,
    GS_REG_PABE = 0x49,
    GS_REG_FBA_1 = 0x4A,
    GS_REG_FBA_2 = 0x4B,
    GS_REG_FRAME_1 = 0x4C,
    GS_REG_FRAME_2 = 0x4D,
    GS_REG_ZBUF_1 = 0x4E,
    GS_REG_ZBUF_2 = 0x4F,
    GS_REG_BITBLTBUF = 0x50,
    GS_REG_TRXPOS = 0x51,
    GS_REG_TRXREG = 0x52,
    GS_REG_TRXDIR = 0x53,
    GS_REG_HWREG = 0x54,
    GS_REG_SIGNAL = 0x60,
    GS_REG_FINISH = 0x61,
    GS_REG_LABEL = 0x62,
};

struct GSVertex
{
    float x = 0.0f;
    float y = 0.0f;
    double z = 0.0;
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 0;
    float q = 1.0f;
    float s = 0.0f;
    float t = 0.0f;
    uint16_t u = 0;
    uint16_t v = 0;
    uint8_t fog = 0;
    // GT4's VU1 tap (gt4_vu1_tap.h; 0 without it): the place of this vertex in its VU1 packet and that packet's tap
    // record slot + 1. In the bytes after fog that padded the struct to 40 already (no size change).
    uint8_t tapK = 0;
    uint16_t tapSlot = 0;
};
static_assert(sizeof(GSVertex) == 40, "GSVertex grew: the tap fields must stay in its padding");

struct GSFrameReg
{
    uint32_t fbp = 0;
    uint32_t fbw = 0;
    uint8_t psm = 0;
    uint32_t fbmsk = 0;
};

struct GSZbufReg
{
    uint32_t zbp = 0;
    uint8_t psm = 0;
    bool zmask = false;
};

struct GSScissorReg
{
    uint16_t x0 = 0;
    uint16_t x1 = 0;
    uint16_t y0 = 0;
    uint16_t y1 = 0;
};

struct GSTex0Reg
{
    uint32_t tbp0 = 0;
    uint8_t tbw = 0;
    uint8_t psm = 0;
    uint8_t tw = 0;
    uint8_t th = 0;
    uint8_t tcc = 0;
    uint8_t tfx = 0;
    uint32_t cbp = 0;
    uint8_t cpsm = 0;
    uint8_t csm = 0;
    uint8_t csa = 0;
    uint8_t cld = 0;
};

struct GSXYOffsetReg
{
    uint16_t ofx = 0;
    uint16_t ofy = 0;
};

struct GSTexaReg
{
    uint8_t ta0 = 0;
    bool aem = false;
    uint8_t ta1 = 0;
};

struct GSTexClutReg
{
    uint8_t cbw = 0;
    uint8_t cou = 0;
    uint16_t cov = 0;
};

struct GSContext
{
    GSFrameReg frame;
    GSScissorReg scissor;
    GSTex0Reg tex0;
    GSXYOffsetReg xyoffset;
    GSZbufReg zbuf;
    uint64_t tex1 = 0;
    uint64_t miptbp1 = 0;
    uint64_t miptbp2 = 0;
    uint64_t clamp = 0;
    uint64_t alpha = 0;
    uint64_t test = 0;
    uint64_t fba = 0;
};

struct GSPrimReg
{
    GSPrimType type = GS_PRIM_POINT;
    bool iip = false;
    bool tme = false;
    bool fge = false;
    bool abe = false;
    bool aa1 = false;
    bool fst = false;
    bool ctxt = false;
    bool fix = false;
};

struct GSBitBltBuf
{
    uint32_t sbp = 0;
    uint8_t sbw = 0;
    uint8_t spsm = 0;
    uint32_t dbp = 0;
    uint8_t dbw = 0;
    uint8_t dpsm = 0;
};

struct GSTrxPos
{
    uint16_t ssax = 0;
    uint16_t ssay = 0;
    uint16_t dsax = 0;
    uint16_t dsay = 0;
    uint8_t dir = 0;
};

struct GSTrxReg
{
    uint16_t rrw = 0;
    uint16_t rrh = 0;
};

struct GSDrawState
{
    GSContext context{};
    GSPrimReg prim{};
    GSTexaReg texa{};
    GSTexClutReg texclut{};
    bool pabe = false;
    uint64_t scanmsk = 0;
    uint64_t dimx = 0;
    uint64_t dthe = 0;
    uint64_t colclamp = 0;
    uint8_t fogR = 0;
    uint8_t fogG = 0;
    uint8_t fogB = 0;
    uint16_t textureWidth = 1;
    uint16_t textureHeight = 1;
    bool linearFilter = false;
    // GS mipmapping (gs_mip.h, set by the frontend before Submit): 0 not
    // resolved; 1 + n: TEX0 / CLAMP region / size / linearFilter are level n's
    // (one level for the whole primitive); 0x80: level per pixel, mipLo..mipHi
    // the levels it can read (TEX0 is level 0's, TEX1 / MIPTBP give the rest).
    uint8_t mipTag = 0;
    uint8_t mipLo = 0;
    uint8_t mipHi = 0;
    // GT4's race colour (gs_fullcolour.h, set by the frontend): 0 GT4's own
    // 16-bit rules; 32: a CT16 / CT16S frame drawn as a CT32 frame would be,
    // its full colour kept beside GS memory (480p and 1080i only).
    uint8_t fc = 0;
    // GT4's race colour (gs_fullcolour.h texFlag, set by the frontend): 1 for a CT16 texture laid out as a
    // display buffer (TBW >= 9, TBP on a page) in 480p / 1080i with race colour 32: each texel is read as its
    // full colour (its plane word while that reduces to the halfword, else the halfword), alpha from bit 15 as
    // always; 0 every other draw (the 16-bit value, as before).
    uint8_t ftex = 0;
    // GT4's race colour: the number of video mode changes (SetGsCrt) the
    // frontend saw before this draw. A renderer that sees a new value forgets
    // every pixel's full colour before drawing (each pixel its 16-bit value
    // again), so full colour lives within one 480p / 1080i session.
    uint16_t fcEpoch = 0;
};

// Field by field (never the padding bytes): whether two primitives' draw
// states are the same. Used by the renderer's draw-state runs (step 7 T1) and
// their statistics. A new GSDrawState field must be added here too: the size
// check below fails until it is.
inline bool gsSameDrawState(const GSDrawState &a, const GSDrawState &b)
{
    const GSContext &x = a.context, &y = b.context;
    return x.frame.fbp == y.frame.fbp && x.frame.fbw == y.frame.fbw && x.frame.psm == y.frame.psm && x.frame.fbmsk == y.frame.fbmsk &&
           x.scissor.x0 == y.scissor.x0 && x.scissor.x1 == y.scissor.x1 && x.scissor.y0 == y.scissor.y0 && x.scissor.y1 == y.scissor.y1 &&
           x.tex0.tbp0 == y.tex0.tbp0 && x.tex0.tbw == y.tex0.tbw && x.tex0.psm == y.tex0.psm && x.tex0.tw == y.tex0.tw &&
           x.tex0.th == y.tex0.th && x.tex0.tcc == y.tex0.tcc && x.tex0.tfx == y.tex0.tfx && x.tex0.cbp == y.tex0.cbp &&
           x.tex0.cpsm == y.tex0.cpsm && x.tex0.csm == y.tex0.csm && x.tex0.csa == y.tex0.csa && x.tex0.cld == y.tex0.cld &&
           x.xyoffset.ofx == y.xyoffset.ofx && x.xyoffset.ofy == y.xyoffset.ofy && x.zbuf.zbp == y.zbuf.zbp &&
           x.zbuf.psm == y.zbuf.psm && x.zbuf.zmask == y.zbuf.zmask && x.tex1 == y.tex1 && x.miptbp1 == y.miptbp1 &&
           x.miptbp2 == y.miptbp2 && x.clamp == y.clamp && x.alpha == y.alpha && x.test == y.test && x.fba == y.fba &&
           a.prim.type == b.prim.type && a.prim.iip == b.prim.iip && a.prim.tme == b.prim.tme && a.prim.fge == b.prim.fge &&
           a.prim.abe == b.prim.abe && a.prim.aa1 == b.prim.aa1 && a.prim.fst == b.prim.fst && a.prim.ctxt == b.prim.ctxt &&
           a.prim.fix == b.prim.fix && a.texa.ta0 == b.texa.ta0 && a.texa.aem == b.texa.aem && a.texa.ta1 == b.texa.ta1 &&
           a.texclut.cbw == b.texclut.cbw && a.texclut.cou == b.texclut.cou && a.texclut.cov == b.texclut.cov && a.pabe == b.pabe &&
           a.scanmsk == b.scanmsk && a.dimx == b.dimx && a.dthe == b.dthe && a.colclamp == b.colclamp && a.fogR == b.fogR &&
           a.fogG == b.fogG && a.fogB == b.fogB && a.textureWidth == b.textureWidth && a.textureHeight == b.textureHeight &&
           a.linearFilter == b.linearFilter && a.mipTag == b.mipTag && a.mipLo == b.mipLo && a.mipHi == b.mipHi && a.fc == b.fc &&
           a.ftex == b.ftex && a.fcEpoch == b.fcEpoch;
}
#if defined(_M_X64) || defined(__x86_64__)
static_assert(sizeof(GSDrawState) == 184 && sizeof(GSContext) == 112 && sizeof(GSTex0Reg) == 20 && sizeof(GSPrimReg) == 9,
              "GSDrawState changed: add the new field to gsSameDrawState and update these sizes");
#endif

struct GSPrimitiveBatch
{
    std::array<GSVertex, 3> vertices{};
    uint8_t vertexCount = 0;
    GSDrawState state{};
};

struct GSTransferCommand
{
    GSBitBltBuf bitbltbuf{};
    GSTrxPos trxpos{};
    GSTrxReg trxreg{};
    uint32_t direction = 3;
};

struct GSTransferSnapshot
{
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t totalPixels = 0;
    uint32_t copiedPixels = 0;
    uint32_t direction = 3;
    size_t localToHostPendingBytes = 0;
};

struct GSPresentationRequest
{
    uint64_t pmode = 0;
    uint64_t smode2 = 0;
    uint64_t dispfb1 = 0;
    uint64_t display1 = 0;
    uint64_t dispfb2 = 0;
    uint64_t display2 = 0;
    uint64_t bgcolor = 0;
    uint64_t vsyncTick = 0;
    GSFrameReg contextFrames[2]{};
    GSFrameReg preferredSource{};
    uint32_t preferredDestFbp = 0;
    bool hasPreferredSource = false;
    // False when the previous VSync's latch was not shown, so its field must
    // not be woven into this one.
    bool allowWeave = true;
    // The shown field: the half-line phase its draws used (fractional OFY of
    // XYOFFSET, 1/16 line; 0xFF unknown) and the write sequence that last
    // changed it (the same field presented again keeps its sequence).
    uint8_t fieldPhase = 0xFFu;
    uint64_t fieldSeq = 0;
    // The video mode SetGsCrt chose when these registers were latched
    // (runtime/gs/gs_crt.h; 0 unknown): 480i, 480p, 1080i ...
    uint32_t crt = 0;
};

struct PresentationFrame
{
    std::vector<uint8_t> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t displayFbp = 0;
    uint32_t sourceFbp = 0;
    bool usedPreferred = false;
    // Composed on the GPU and handed to the host through gs_gl_present
    // (pixels stay empty).
    bool gpuFrame = false;
    // The request's video mode (gs_crt.h), for the host's picture shape and
    // scanlines.
    uint32_t crt = 0;

    explicit operator bool() const
    {
        return (gpuFrame || !pixels.empty()) && width != 0u && height != 0u;
    }
};

enum class GSSyncReason : uint8_t
{
    Finish,
    LocalToHost,
    Presentation,
    DebugReadback,
    Reset,
};
