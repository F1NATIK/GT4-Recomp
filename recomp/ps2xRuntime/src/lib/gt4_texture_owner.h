#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

// GT4_TEXTURE_DUMP's car and course folders (gt4_texture_dump.h): which car or course a dumped texture belongs to.
// Development and Full GT4.exe only (the Classic build has stubs). Nothing here changes what GT4 reads or draws.
//
// How a texture is tied to a car or a course:
//   1. The disc: the IOP tells this file every read of the disc image (ps2x/iop/gt4_disc_reads.h). Reads inside a
//      car's files (GT4.VOL car/menu/<model>, car/lod/<model>, car/open/<model>), a course (crs/<course>,
//      crs/<course>.ad) or the wheels and tyres (wheel/..., tire/...: one shared owner, since GT4 reads them after all
//      of a race's cars and several cars share them) are queued;
//      a thread reads each such file once from the disc image itself, unpacks it and indexes its content: a hash of
//      every 16-byte aligned 32-byte window -> the car or course (a window found in two owners' files counts for
//      neither).
//   2. The uploads: GT4 streams its textures into GS memory as host -> local transfers of the bytes its model and
//      course files hold. The GS renderer thread hashes up to 8 windows of each transfer's data at the same 16-byte
//      steps and keeps them for every 8 KB GS page the transfer writes (the last transfer into a page wins).
//   3. The dump: a texture's first draw copies the windows of the page at its TBP; when the picture is written the
//      windows are looked up and the owner most of them name is its owner. None: "other".
// Names: the car's model code (car/lod/toyt0032) -> GT4's VARIATION table (the colour group whose model is that
// code) -> the car (CAR_VARIATION_american) -> its label (castrol_supra_00) and full name (CAR_NAME_american:
// "Toyota Castrol Tom's Supra (JGTC) '00"); the course's label (trialmt) -> GT4's own text (rtext CourseName:
// "Trial Mountain Circuit"). All read from the player's disc image at run time (gt4stock_names / gt4save, the
// launcher's readers); a name the disc does not give: the label or the model code.
namespace gt4texowner
{
    constexpr int kSamples = 8;
    struct Sample
    {
        std::array<uint64_t, kSamples> hash{};
        uint8_t count = 0;
    };

    struct Owner
    {
        enum Kind
        {
            None,
            Car,
            Track,
            Wheels // wheel/ and tire/ files (shared by cars)
        } kind = None;
        std::string folder; // the folder name: the car's / course's name, else its label (file-name safe)
        std::string key;    // "car:castrol_supra_00" / "track:trialmt" / "wheels" (index.csv)
    };

    // Turned on by the texture dump (GT4_TEXTURE_DUMP): the disc observer and the indexing thread.
    void start();
    // The GS renderer thread: a host -> local transfer begins (the GS pages it writes, its size in bytes), its data.
    void beginUpload(const std::vector<uint32_t> &pages, uint64_t bytes);
    void uploadData(const uint8_t *data, uint32_t bytes);
    // The GS renderer thread: the windows of the transfer that last wrote the page holding GS block tbp.
    Sample sampleAt(uint32_t tbp);
    // Any thread: the owner the windows name (waits up to 2 s for files still being indexed).
    Owner resolve(const Sample &sample);
    void shutdown();
}
