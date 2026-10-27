#include "runtime/gt3_movie_queue.h"
#include <algorithm>

namespace ps2x::video {
bool Gt3MovieQueue::configure(uint32_t width, uint32_t height, uint32_t capacity) {
    if (!width || !height || width > 4096 || height > 4096 || width % 16 || height % 16 ||
        !capacity || capacity > 8 || uint64_t(width) * height * 4 * capacity > 64 * 1024 * 1024) return false;
    std::vector<Gt3MoviePictureLease> fresh(capacity);
    slots_.swap(fresh);
    width_ = width; height_ = height; write_ = count_ = 0;
    cancelled_ = primed_ = false;
    return true;
}
Gt3MoviePublish Gt3MovieQueue::publish(const MpegDecodedFrame& frame) {
    if (cancelled_) return Gt3MoviePublish::Cancelled;
    if (slots_.empty() || frame.width != int(width_) || frame.height != int(height_) ||
        frame.rgba.size() != size_t(width_) * height_ * 4) return Gt3MoviePublish::Invalid;
    if (count_ == slots_.size()) return Gt3MoviePublish::Full;
    auto picture = std::make_shared<Gt3MoviePicture>();
    picture->width = width_; picture->height = height_;
    picture->pts90k = frame.pts90k; picture->repeatPict = frame.repeatPict;
    picture->tiledRgba.resize(frame.rgba.size());
    size_t at = 0;
    for (uint32_t x = 0; x < width_; x += 16)
        for (uint32_t y = 0; y < height_; y += 16)
            for (uint32_t row = 0; row < 16; ++row) {
                const auto source = (size_t(y + row) * width_ + x) * 4;
                std::copy_n(frame.rgba.data() + source, 64, picture->tiledRgba.data() + at);
                at += 64;
            }
    // Publish only after all pixels and metadata have been constructed.
    slots_[write_] = std::move(picture);
    write_ = (write_ + 1) % uint32_t(slots_.size());
    if (++count_ == slots_.size()) primed_ = true;
    return Gt3MoviePublish::Published;
}
Gt3MoviePictureLease Gt3MovieQueue::front() const {
    if (cancelled_ || !count_) return {};
    return slots_[(write_ + slots_.size() - count_) % slots_.size()];
}
bool Gt3MovieQueue::release(const Gt3MoviePictureLease& expected) {
    if (!expected || front() != expected) return false;
    slots_[(write_ + slots_.size() - count_) % slots_.size()].reset();
    --count_;
    return true;
}
void Gt3MovieQueue::cancel() {
    for (auto& slot : slots_) slot.reset();
    count_ = write_ = 0; primed_ = false; cancelled_ = true;
}
}
