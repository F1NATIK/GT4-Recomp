// GT4 movie stutter fix (analysis/design/movie-stutter/REPORT.md): the movie producer's copy of each video record into
// the IPU video buffer, done natively. Both editions; GT4_MOVIE_FEED_COPY=0 turns it off (GT4's own byte loop).
#pragma once

class PS2Runtime;

namespace gt4movie
{
// After CORE.elf is loaded: checks the three call sites in GT4's movie feed (0x54fbb8) and GT4's memcpy (0x5a4724),
// then puts the hook on 0x5a4724 (every other caller runs GT4's own memcpy unchanged).
void installFeedCopy(PS2Runtime &runtime);
}
