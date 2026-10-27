#pragma once

// GT4's race colour (gs_fullcolour.h): the plane index of a CT16 / CT16S pixel, for the renderers (the GS
// frontend needs only gs_fullcolour.h).

#include "runtime/gs/gs_fullcolour.h"
#include "runtime/gs/ps2_gs_memory.h"

namespace gsfc
{
    // The plane index (GS memory halfword) of pixel (x, y) of a CT16 / CT16S buffer at block bp, width bw.
    inline uint32_t halfIndex(uint32_t psm, uint32_t bp, uint32_t bw, uint32_t x, uint32_t y)
    {
        using namespace GSMem;
        const uint32_t a = psm == GS_PSM_CT16S ? PixelStorageTraits<C16S>::Address(PageTableC16S, bp, bw, x, y)
                                               : PixelStorageTraits<C16>::Address(PageTableC16, bp, bw, x, y);
        return a & static_cast<uint32_t>(kPlaneWords - 1u);
    }
}
