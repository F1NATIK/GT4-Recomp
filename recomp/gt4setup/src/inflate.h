#pragma once
#include "util.h"

namespace gt4setup {

struct InflateResult {
    Bytes data;
    bool ended = false;   // the final block's end was reached
    size_t consumed = 0;  // input bytes used
    std::string error;    // non-empty: corrupt stream
};

// Raw DEFLATE (no zlib/gzip header). Stops early, with ended = false, once
// maxOut bytes have been produced and more would follow.
InflateResult inflateRaw(const uint8_t *in, size_t inLen, size_t maxOut);

} // namespace gt4setup
