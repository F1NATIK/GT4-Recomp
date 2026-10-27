#include "runtime/gt3_movie_session.h"
#include "runtime/mpeg_video_packet.h"
namespace ps2x::video {
void Gt3MovieSession::stop() {
    output_.stop(); packed_.cancel(); pending_.clear(); active_ = false;
}
Gt3MovieStep Gt3MovieSession::fail(const std::string& message) {
    error_ = message; stop(); return Gt3MovieStep::Failed;
}
uint64_t Gt3MovieSession::start(uint32_t descriptor, uint32_t priming, uint32_t width, uint32_t height) {
    stop(); error_.clear(); decoder_.reset(); ending_ = false; pictures_ = 0;
    if (!MpegVideoDecoder::available() || !packed_.configure(width, height, 1)) {
        fail("Native movie decoder or picture geometry unavailable"); return 0;
    }
    session_ = output_.bind(descriptor, priming, width, height);
    if (!session_) { fail("Invalid guest movie output queue"); return 0; }
    active_ = true; return session_;
}
Gt3MovieStep Gt3MovieSession::pump(uint64_t session) {
    if (!active_ || session != session_) return Gt3MovieStep::Stopped;
    if (!output_.isActive(session_)) { stop(); return Gt3MovieStep::Stopped; }
    while (packed_.count() || !pending_.empty()) {
        if (!packed_.count()) {
            if (packed_.publish(pending_.front()) != Gt3MoviePublish::Published)
                return fail("Decoded picture does not match the guest movie queue");
            pending_.pop_front();
        }
        auto picture = packed_.front();
        switch (output_.publish(session_, *picture)) {
        case Gt3GuestPictureResult::WouldBlock: return Gt3MovieStep::NeedOutput;
        case Gt3GuestPictureResult::Stopped: stop(); return Gt3MovieStep::Stopped;
        case Gt3GuestPictureResult::Invalid: return fail("Guest movie output queue became invalid");
        case Gt3GuestPictureResult::Published:
            if (!packed_.release(picture)) return fail("Native picture ownership mismatch");
            ++pictures_; break;
        }
    }
    return ending_ ? Gt3MovieStep::InputDrained : Gt3MovieStep::Ready;
}
Gt3MovieStep Gt3MovieSession::feedPes(uint64_t session, std::span<const uint8_t> packet) {
    if (!active_ || session != session_) return Gt3MovieStep::Stopped;
    if (!output_.isActive(session_)) { stop(); return Gt3MovieStep::Stopped; }
    if (ending_) return fail("Video packet supplied after end of input");
    const auto ready = pump(session);
    if (ready != Gt3MovieStep::Ready) return ready;
    MpegVideoPes pes;
    if (packet.size() > 65541 || !parseMpegVideoPes(packet, pes)) return fail("Invalid video PES packet");
    if (!decoder_.feed(pes.payload.data(), pes.payload.size(), pending_, pes.pts90k, pes.dts90k))
        return fail(decoder_.error());
    const auto result = pump(session);
    if (result == Gt3MovieStep::Failed || result == Gt3MovieStep::Stopped) return result;
    // Output may be full, but these compressed bytes already entered the codec.
    return Gt3MovieStep::Accepted;
}
Gt3MovieStep Gt3MovieSession::feedRecord(uint64_t session, std::span<const uint8_t> record) {
    if (!active_ || session != session_) return Gt3MovieStep::Stopped;
    if (!output_.isActive(session_)) { stop(); return Gt3MovieStep::Stopped; }
    const auto parsed = parseGt3MovieRecord(record);
    if (parsed.kind == Gt3MovieRecordKind::Invalid) return fail("Invalid GT3 movie record");
    if (parsed.kind == Gt3MovieRecordKind::Boundary) return Gt3MovieStep::Boundary;
    return feedPes(session, parsed.packet);
}
Gt3MovieStep Gt3MovieSession::finish(uint64_t session) {
    if (!active_ || session != session_) return Gt3MovieStep::Stopped;
    if (!output_.isActive(session_)) { stop(); return Gt3MovieStep::Stopped; }
    auto ready = pump(session);
    if (ready == Gt3MovieStep::Ready) {
        if (!decoder_.flush(pending_)) return fail(decoder_.error());
        ending_ = true; ready = pump(session);
    }
    if (ready != Gt3MovieStep::InputDrained) return ready;
    switch (output_.primeAtEnd(session)) {
    case Gt3GuestDrainResult::Busy: return Gt3MovieStep::NeedOutput;
    case Gt3GuestDrainResult::Stopped: stop(); return Gt3MovieStep::Stopped;
    case Gt3GuestDrainResult::Invalid: return fail("Guest movie queue invalid at end of input");
    case Gt3GuestDrainResult::Empty:
    case Gt3GuestDrainResult::Pending: return Gt3MovieStep::InputDrained;
    }
    return fail("Unknown movie end state");
}
Gt3MovieStep Gt3MovieSession::drain(uint64_t session) {
    const auto state = finish(session);
    if (state != Gt3MovieStep::InputDrained) return state;
    switch (output_.drainState(session)) {
    case Gt3GuestDrainResult::Empty: return Gt3MovieStep::Complete;
    case Gt3GuestDrainResult::Pending:
    case Gt3GuestDrainResult::Busy: return Gt3MovieStep::NeedOutput;
    case Gt3GuestDrainResult::Stopped: stop(); return Gt3MovieStep::Stopped;
    case Gt3GuestDrainResult::Invalid: return fail("Guest movie queue invalid while draining");
    }
    return fail("Unknown movie drain state");
}
Gt3MovieStep Gt3MovieSession::repeat(uint64_t session) {
    if (!active_ || session != session_) return Gt3MovieStep::Stopped;
    if (!output_.isActive(session_)) { stop(); return Gt3MovieStep::Stopped; }
    // A caller must explicitly finish the previous pass; never discard pending
    // compressed input or pictures to make room for a new one.
    if (!ending_) return fail("Movie repeat requested before end of input");
    const auto state = pump(session);
    if (state != Gt3MovieStep::InputDrained) return state;
    switch (output_.drainState(session)) {
    case Gt3GuestDrainResult::Busy: return Gt3MovieStep::NeedOutput;
    case Gt3GuestDrainResult::Stopped: stop(); return Gt3MovieStep::Stopped;
    case Gt3GuestDrainResult::Invalid: return fail("Guest movie queue invalid before repeat");
    case Gt3GuestDrainResult::Empty:
    case Gt3GuestDrainResult::Pending: break;
    }
    decoder_.reset(); ending_ = false; return Gt3MovieStep::Ready;
}
}
