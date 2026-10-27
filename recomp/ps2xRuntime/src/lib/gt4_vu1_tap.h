#pragma once

// The VU1 tap (lighting step 1; analysis/design/lighting/research/VU1-TAP-RESEARCH.md): per VU1 program, GT4's own
// per-vertex input (local positions, normals) and its local->clip matrix, read before the program runs (the lit
// modules overwrite the normals in VU memory), and the XGKICK packet that program's kernel sends, matched vertex for
// vertex. Read-only on VU memory and registers; nothing it does reaches the picture. Full edition only.
//   GT4_VU1_TAP=1: on (records kept for the renderer, later steps); =verify: also re-computes every mapped vertex's
//   screen position from the tapped input and the matrix and checks it against the packet's XYZF2 (logged counts).

#include <cstdint>

struct VU1State;

namespace gt4tap
{
    // On (GT4_VU1_TAP set, Full edition): read once.
    bool enabled();

    // runVu1, before the program runs (MSCAL; resume = MSCNT is not tapped).
    void beginProgram(const VU1State &vu, uint32_t startPc, uint32_t top, uint32_t itop, const uint8_t *vuData, uint64_t vsync);
    // Every XGKICK packet of that program (from the packet sink; never consumes it).
    void packet(const uint8_t *data, uint32_t size);
    // runVu1, after the program.
    void endProgram();

    // A mapped program's record (slot = the packet's GIF tag bits 16-31 minus 1, carried to each GSVertex as tapSlot,
    // the vertex's place in the packet as tapK). Written on the VU1 thread before its packet is sent; a ring, so a
    // record lives ~10 frames.
    struct Record
    {
        uint64_t serial = 0;
        uint32_t count = 0;
        bool hasNormal = false;
        int8_t module = -1;
        float m[4][4] = {};
        float vs[4] = {}, vo[4] = {};
        float pos[64][3];
        float nrm[64][3];
        // Lighting step 1c: the normals in view space (rows: the matrix's x, y and -w columns, normalised; exact for
        // rigid objects, the cars), and each vertex's screen position (GS pixels) a frame earlier from the matched
        // group's previous matrix (hasPrev; motion = now - prev).
        bool hasPrev = false;
        float viewN[64][3];
        float prev[64][2];
        // The VU1 frame the program ran in (DLSS: every vertex of a frame gets that frame's jitter).
        uint64_t frame = 0;
        // Lighting step 2: a car body lit by M07 (lit): its light set (3 rows of 4 light directions in local space, one
        // light per lane; the ambient colour; 4 light colours; the clamp). GT4's colour per vertex is
        // min(max(ambient + sum colour_i * max(L_i . n, 0), 0), clamp) with n = nrm (checked exact, step 2a).
        // M01-M03 (race cars' other lit parts) are M07 with lights 3 and 4 raised to a power (4, 8, 16): lightPow.
        bool lit = false;
        float light[3][4], ambient[4], lightColour[4][4], lightClamp[4], lightPow;
        // Lighting step 2c: a car's reflection layer (M10, env): the eye in local space, the local->view rotation, the
        // colour factor (constant, slope) and each vertex's input colour. GT4 per vertex (listing 0x10a8-0x1268):
        // V = pos - eye, R = V - 2 (V.n) n, r = rotation R / |V|, r.z += 1, UV = (0.5 + r.x / 2|r|, 0.5 - r.y / 2|r|);
        // colour = input x (constant + slope min(V.n, 0) / |V|).
        bool env = false;
        float eye[4], envRot[3][4], fresnel[2][4];
        uint8_t inColour[64][4];
    };
    // M10's reflection for one vertex (UV, colour before truncation), as above.
    void envVertex(const Record &r, uint32_t k, float &u, float &v, float colour[4]);
    // A tapped vertex for the lighting steps: its program's record, local position, normal and input colour (M10). A
    // clipper vertex (near the camera, cut by GT4's clipper) is unprojected and put on the program's own triangle it lies
    // on (the strip's triangles; the closest), its normal and colour interpolated there.
    struct Corner
    {
        const Record *rec = nullptr;
        float pos[3], nrm[3], colour[4];
    };
    bool corner(uint16_t mark, uint32_t k, float x, float y, float q, Corner &out);
    // A local direction in the view space of the record's matrix (rows: its x, y and -w columns, normalised; as viewN).
    void viewNormal(const Record &r, const float n[3], float out[3]);
    // A marked vertex's program's depth mapping: GS Z (/ 2^24) = A + B Q, from its matrix (clip z against clip w) and the
    // viewport's z lane. GT4 draws its scenery in depth partitions (B about 10 far, 1, 0.1 near) with overlapping Z.
    bool zMapping(uint16_t mark, double &A, double &B);
    // Mark (a packet's GIF tag bits 16-31, carried to each GSVertex as tapSlot): 0 none; 1..0x7FFF a record (slot =
    // mark - 1); 0x8000 | n a clipper record (DLSS step 1): the clipper's packets (triangles the kernel did not send
    // whole, mostly near the camera) carry no input vertices, so their vertices' last-frame positions come from their
    // screen position and Q (= 1 / clip w, checked for every kernel vertex) back through the matrix and on through
    // last frame's.
    constexpr uint16_t kClipMark = 0x8000u;
    const Record *record(uint32_t slot);
    // The renderer's motion pass wanted (DLSS, the checks): every mode but per-pixel lighting alone.
    bool motion();
    // A marked vertex (GS pixels, its Q, its place in the packet) a frame earlier; false: no history, behind the
    // camera then, or not marked. frame: the VU1 frame of its program.
    bool vertexPrev(uint16_t mark, uint32_t k, float x, float y, float q, float &px, float &py);
    bool markFrame(uint16_t mark, uint64_t &frame);
    // Relighting (GT4_RELIGHT): whether a marked vertex is a car's (its program reads normals: the lit and reflection
    // modules); GT4's sun (light 0 of the newest car body's light set) and the world's up (that car's local +y), each in the
    // renderer's view space (x and y as GS pixels grow, z = clip w, the distance), and the VU1 frame they came from.
    bool carMark(uint16_t mark);
    bool sun(float L[3], float up[3], uint64_t &frame);
    bool verifying();
    // GT4_VU1_TAP=normals / motion: the renderer shows tapped vertices' view normals / motion as colours (debug).
    int debugView(); // 0 none, 1 normals, 2 motion
    // GT4_VU1_TAP=verify: the GS's vertex (GS pixels) against its record (logged now and then).
    void checkVertex(uint32_t slot, uint32_t k, float gsX, float gsY);
    // A PNG of RGBA8 pixels (debug pictures).
    bool savePng(const char *path, int width, int height, const uint8_t *rgba);
} // namespace gt4tap
