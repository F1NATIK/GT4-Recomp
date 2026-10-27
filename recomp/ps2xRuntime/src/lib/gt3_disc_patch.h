#pragma once

#include <cstddef>
#include <cstdint>

// GT3_DISC_PATCH=<folder> (development build only; off and free without it): byte ranges of the disc image
// replaced as they are read (the IOP host's reads of the CD image and the EE CD stub's). The folder holds
// patches.txt, one "<image byte offset, hex> <file name>" per line ('#' = comment); each file's bytes stand in
// for the image's from that offset. Made for giving GT3 converted files in place of its own without touching
// the disc image: a GT3.VOL file's sectors plus its TOC node's sizes (V8SC3-WORK\tools\make_disc_patch.py,
// first use: a Holden Commodore VZ body in place of the Corvette C5R's cars/day/cv0024).
namespace gt3discpatch
{
    bool enabled();
    void overlay(uint64_t imageOffset, uint8_t *destination, size_t size);
}
