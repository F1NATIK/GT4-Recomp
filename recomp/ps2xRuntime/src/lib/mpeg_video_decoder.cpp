#include "runtime/mpeg_video_decoder.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>

#if PS2X_HAS_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libswscale/swscale.h>
}
#endif

namespace ps2x::video {
struct MpegVideoDecoder::State {
    std::string error;
    bool drained=false;
    bool fail(const std::string& message) {
        if (error.empty()) { error=message; std::cerr<<"[MPEG] "<<error<<'\n'; }
        return false;
    }
#if PS2X_HAS_FFMPEG
    AVCodecParserContext* parser=nullptr;
    AVCodecContext* codec=nullptr;
    AVPacket* packet=nullptr;
    AVFrame* frame=nullptr;
    SwsContext* scaler=nullptr;
    std::vector<uint8_t> padded;
    ~State() {
        sws_freeContext(scaler);
        av_frame_free(&frame); av_packet_free(&packet); avcodec_free_context(&codec);
        if (parser) av_parser_close(parser);
    }
    bool failCode(const char* operation,int code) {
        std::array<char,AV_ERROR_MAX_STRING_SIZE> message{};
        av_strerror(code,message.data(),message.size());
        return fail(std::string(operation)+": "+message.data());
    }
    bool initialize() {
        if (codec) return true;
        const auto* decoder=avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO);
        if (!decoder) return fail("MPEG-2 decoder unavailable");
        parser=av_parser_init(AV_CODEC_ID_MPEG2VIDEO);
        codec=avcodec_alloc_context3(decoder); packet=av_packet_alloc(); frame=av_frame_alloc();
        if (!parser || !codec || !packet || !frame) return fail("decoder allocation failed");
        codec->thread_count=1;
        codec->pkt_timebase={1,90000};
        codec->max_pixels=4096*4096;
        // Keep P/B pictures; do not silently accept damaged output.
        codec->skip_frame=AVDISCARD_DEFAULT;
        // GT3 pads completed slices with zero bits. AV_EF_BITSTREAM rejects
        // more than eight remaining bits even when they are zero (FFmpeg7.1
        // mpeg12dec.c, mpeg_decode_slice). Keep buffer/error checks and fail on
        // decode errors, without imposing that incompatible padding restriction.
        codec->err_recognition=AV_EF_BUFFER|AV_EF_EXPLODE;
        const int result=avcodec_open2(codec,decoder,nullptr);
        return result>=0 || failCode("open decoder",result);
    }
    bool receive(std::deque<MpegDecodedFrame>& frames) {
        for (;;) {
            const int result=avcodec_receive_frame(codec,frame);
            if (result==AVERROR(EAGAIN) || result==AVERROR_EOF) return true;
            if (result<0) return failCode("receive picture",result);
            if (frame->decode_error_flags || (frame->flags&AV_FRAME_FLAG_CORRUPT)) {
                av_frame_unref(frame); return fail("decoder returned a corrupt picture");
            }
            const int width=frame->width,height=frame->height;
            if (width<=0 || height<=0 || width>4096 || height>4096 || frames.size()>=64) {
                av_frame_unref(frame); return fail("picture dimensions or output queue exceed bounds");
            }
            scaler=sws_getCachedContext(scaler,width,height,static_cast<AVPixelFormat>(frame->format),
                width,height,AV_PIX_FMT_RGBA,SWS_BILINEAR,nullptr,nullptr,nullptr);
            if (!scaler) { av_frame_unref(frame); return fail("RGBA scaler allocation failed"); }
            MpegDecodedFrame decoded;
            decoded.width=width; decoded.height=height; decoded.repeatPict=std::max(0,frame->repeat_pict);
            decoded.pts90k=frame->best_effort_timestamp==AV_NOPTS_VALUE ? -1 : frame->best_effort_timestamp;
            decoded.rgba.resize(size_t(width)*height*4);
            uint8_t* destination[4]={decoded.rgba.data(),nullptr,nullptr,nullptr};
            const int stride[4]={width*4,0,0,0};
            const int rows=sws_scale(scaler,frame->data,frame->linesize,0,height,destination,stride);
            av_frame_unref(frame);
            if (rows!=height) return fail("incomplete RGBA conversion");
            frames.push_back(std::move(decoded));
        }
    }
    bool send(const uint8_t* bytes,int size,std::deque<MpegDecodedFrame>& frames) {
        av_packet_unref(packet);
        const int allocation=av_new_packet(packet,size);
        if (allocation<0) return failCode("allocate packet",allocation);
        std::memcpy(packet->data,bytes,size);
        packet->pts=parser->pts; packet->dts=parser->dts;
        int result=avcodec_send_packet(codec,packet);
        if (result==AVERROR(EAGAIN)) {
            if (!receive(frames)) { av_packet_unref(packet); return false; }
            result=avcodec_send_packet(codec,packet);
        }
        av_packet_unref(packet);
        if (result<0) return failCode("send packet",result);
        return receive(frames);
    }
#endif
};

MpegVideoDecoder::MpegVideoDecoder():state_(std::make_unique<State>()) {}
MpegVideoDecoder::~MpegVideoDecoder()=default;
bool MpegVideoDecoder::available() {
#if PS2X_HAS_FFMPEG
    return avcodec_find_decoder(AV_CODEC_ID_MPEG2VIDEO)!=nullptr;
#else
    return false;
#endif
}
const std::string& MpegVideoDecoder::error() const { return state_->error; }
void MpegVideoDecoder::reset() { state_=std::make_unique<State>(); }
bool MpegVideoDecoder::feed(const uint8_t* bytes,size_t size,std::deque<MpegDecodedFrame>& frames,
                            int64_t pts90k,int64_t dts90k) {
    auto& s=*state_;
    if (!s.error.empty()) return false;
    if (s.drained) return s.fail("input after flush requires reset");
    if (!size) return true;
    if (!bytes || size>4*1024*1024) return s.fail("invalid input span or feed exceeds 4 MiB");
#if PS2X_HAS_FFMPEG
    if (!s.initialize()) return false;
    // FFmpeg may read AV_INPUT_BUFFER_PADDING_SIZE bytes beyond the logical
    // input. Never pass a guest/ring span directly to its bitstream parser.
    s.padded.assign(size+AV_INPUT_BUFFER_PADDING_SIZE,0);
    std::memcpy(s.padded.data(),bytes,size);
    size_t offset=0;
    int noProgress=0;
    while (offset<size) {
        uint8_t* packetData=nullptr; int packetBytes=0;
        const int used=av_parser_parse2(s.parser,s.codec,&packetData,&packetBytes,
            s.padded.data()+offset,int(size-offset),pts90k<0?AV_NOPTS_VALUE:pts90k,
            dts90k<0?AV_NOPTS_VALUE:dts90k,AV_NOPTS_VALUE);
        if (used<0) return s.failCode("parse input",used);
        if (size_t(used)>size-offset || (!used && !packetBytes) || (!used && ++noProgress>2))
            return s.fail("parser made no bounded progress");
        offset+=used;
        if (used) { pts90k=dts90k=-1; noProgress=0; }
        if (packetBytes && !s.send(packetData,packetBytes,frames)) return false;
    }
    return true;
#else
    return s.fail("runtime built without native MPEG video decoding");
#endif
}
bool MpegVideoDecoder::flush(std::deque<MpegDecodedFrame>& frames) {
    auto& s=*state_;
    if (!s.error.empty()) return false;
    if (s.drained) return true;
#if PS2X_HAS_FFMPEG
    if (s.codec) {
        uint8_t* data=nullptr; int size=0;
        const int parsed=av_parser_parse2(s.parser,s.codec,&data,&size,nullptr,0,AV_NOPTS_VALUE,AV_NOPTS_VALUE,AV_NOPTS_VALUE);
        if (parsed<0) return s.failCode("flush parser",parsed);
        if (size && !s.send(data,size,frames)) return false;
        int result=avcodec_send_packet(s.codec,nullptr);
        if (result==AVERROR(EAGAIN)) {
            if (!s.receive(frames)) return false;
            result=avcodec_send_packet(s.codec,nullptr);
        }
        if (result<0 && result!=AVERROR_EOF) return s.failCode("flush decoder",result);
        if (!s.receive(frames)) return false;
    }
#endif
    s.drained=true; return true;
}
}
