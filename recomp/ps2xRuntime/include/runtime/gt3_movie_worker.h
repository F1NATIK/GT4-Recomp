#pragma once
#include "runtime/gt3_movie_session.h"
struct R5900Context;
namespace ps2x::video {
// EE-executor-owned adapter for the hash-verified USA CORE movie routines.
// Initialization/cleanup stay in 25f5f8; 260998 and 25f6e0 use this one producer.
class Gt3MovieWorker {
public:
    explicit Gt3MovieWorker(PS2Runtime& runtime) : runtime_(runtime), session_(runtime) {}
    void reset();
    void engine(R5900Context& context);
    uint32_t feed(uint32_t decoder, uint32_t record);
    const std::string& error() const { return error_; }
    uint64_t picturesPublished() const { return session_.picturesPublished(); }
private:
    void fail(const std::string& message);
    void wakeProducer();
    uint32_t terminalFeed() const;
    PS2Runtime& runtime_;
    Gt3MovieSession session_;
    uint64_t token_ = 0;
    int owner_ = 0;
    bool ended_ = false, boundary_ = false, stopping_ = false, playing_ = false;
    std::string error_;
};
bool tryGt3MovieEngine(PS2Runtime& runtime, R5900Context& context);
bool tryGt3MovieFeed(PS2Runtime& runtime, R5900Context& context);
// Original25ed30 waits for old-IPU input occupancy, which native decoding never
// fills. It has no static caller in the verified CORE; reaching it is a hard error.
bool trapGt3MovieBufferWait(PS2Runtime& runtime, R5900Context& context);
void beginGt3MovieWorker(PS2Runtime& runtime);
}
