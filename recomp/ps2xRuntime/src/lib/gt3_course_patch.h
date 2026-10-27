#pragma once

#include <cstdint>
#include <string>

class PS2Runtime;

// GT3_RUNWAY_REPLACE=<folder> (development build only; GT4 track-import
// research, see the GT3 recomp): a course's runway (GTRW, course
// slot 28: road faces, walls, progress lines, grid) is replaced as GT3's
// course loader (0x1A8D18) starts on it. The folder holds `<crc32>.gtrw`
// files, the CRC32 being that of the GT3 runway section they replace (its
// whole section: +0x0C + 16 bytes), so only that exact course is touched.
// A replacement is used only if it fits the original section.
// GT3_COURSE_REPLACE=<folder> of `<crc32>.crs` (same key: the GT3 course's
// runway CRC32): whole course files in GT3's layout, up to GT3's 0xD00000-byte
// course buffer (GT4 models converted to GTM1 etc.; GT4-WORK\tools\course_build.py).
namespace gt3coursepatch
{
    void install(PS2Runtime &runtime, const uint8_t *rdram);
    // The course GT3 loaded last (the hook also runs for GT3_DRIVE_LOG): the
    // CRC32 of its own runway section, and the GT3_COURSE_REPLACE folder's name
    // when a whole replacement course was loaded in its place (else empty).
    // False before any course loaded.
    bool lastCourse(uint32_t &runwayCrc, std::string &replacement);
}
