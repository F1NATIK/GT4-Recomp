#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

namespace ps2x::video {
struct MpegDecodedFrame {
    int width=0, height=0, repeatPict=0;
    int64_t pts90k=-1;
    std::vector<uint8_t> rgba;
};

// Native MPEG-2 elementary-stream decoding. One owner/thread per instance;
// callers remove delivered frames between feeds. No PS2 hardware or guest code.
// Errors are terminal until reset; flush drains delayed pictures exactly once.
class MpegVideoDecoder {
public:
    MpegVideoDecoder();
    ~MpegVideoDecoder();
    MpegVideoDecoder(const MpegVideoDecoder&)=delete;
    MpegVideoDecoder& operator=(const MpegVideoDecoder&)=delete;
    static bool available();
    bool feed(const uint8_t*, size_t, std::deque<MpegDecodedFrame>&,
              int64_t pts90k=-1, int64_t dts90k=-1);
    bool flush(std::deque<MpegDecodedFrame>&);
    void reset();
    const std::string& error() const;
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
