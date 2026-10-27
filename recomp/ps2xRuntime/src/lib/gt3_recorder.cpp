#include "gt3_recorder.h"

#include "external/glad.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if PS2X_HAS_FFMPEG
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}
#endif

namespace
{
    const char *recordPath()
    {
        static const char *path = [] {
            const char *v = std::getenv("GT3_RECORD");
            return (v && *v) ? v : nullptr;
        }();
        return path;
    }

#if PS2X_HAS_FFMPEG
    struct Picture
    {
        std::vector<uint8_t> rgba;
        int64_t ms = 0;
        uint64_t tick = 0;
    };

    // The encoder thread: RGBA pictures in, packets out to the file.
    class Encoder
    {
    public:
        bool open(const char *path, int width, int height)
        {
            m_width = width;
            m_height = height;
            if (avformat_alloc_output_context2(&m_format, nullptr, nullptr, path) < 0 || !m_format)
            {
                std::fprintf(stderr, "[record] no container for %s (use .mp4 or .mkv)\n", path);
                return false;
            }
            // The GPU's encoder first; MPEG-4 on the CPU when there is none.
            const struct
            {
                const char *name;
                AVPixelFormat format;
            } kCodecs[] = {{"h264_nvenc", AV_PIX_FMT_NV12}, {"h264_mf", AV_PIX_FMT_NV12}, {"mpeg4", AV_PIX_FMT_YUV420P}};
            for (const auto &c : kCodecs)
                if (openCodec(c.name, c.format))
                    break;
            if (!m_codec)
            {
                std::fprintf(stderr, "[record] no video encoder could be opened\n");
                return false;
            }
            m_stream = avformat_new_stream(m_format, nullptr);
            avcodec_parameters_from_context(m_stream->codecpar, m_codec);
            m_stream->time_base = m_codec->time_base;
            if (!(m_format->oformat->flags & AVFMT_NOFILE) && avio_open(&m_format->pb, path, AVIO_FLAG_WRITE) < 0)
            {
                std::fprintf(stderr, "[record] cannot write %s\n", path);
                return false;
            }
            // Fragmented MP4: the file plays up to the last fragment even if
            // the game does not exit cleanly.
            AVDictionary *options = nullptr;
            if (std::strcmp(m_format->oformat->name, "mp4") == 0 || std::strcmp(m_format->oformat->name, "mov") == 0)
                av_dict_set(&options, "movflags", "frag_keyframe+empty_moov+default_base_moof", 0);
            const int header = avformat_write_header(m_format, &options);
            av_dict_free(&options);
            if (header < 0)
            {
                std::fprintf(stderr, "[record] cannot start %s\n", path);
                return false;
            }
            m_frame = av_frame_alloc();
            m_frame->format = m_codec->pix_fmt;
            m_frame->width = width;
            m_frame->height = height;
            av_frame_get_buffer(m_frame, 0);
            m_packet = av_packet_alloc();
            m_sws = sws_getContext(width, height, AV_PIX_FMT_RGBA, width, height, m_codec->pix_fmt, SWS_POINT, nullptr, nullptr, nullptr);
            m_csv = std::fopen((std::string(path) + ".frames.csv").c_str(), "w");
            if (m_csv)
                std::fputs("frame,ms,vsync\n", m_csv);
            std::fprintf(stderr, "[record] %s: %dx%d, %s\n", path, width, height, m_codec->codec->name);
            m_thread = std::thread([this] { run(); });
            return true;
        }

        std::vector<uint8_t> takeBuffer()
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_free.empty())
                return std::vector<uint8_t>(static_cast<size_t>(m_width) * m_height * 4u);
            std::vector<uint8_t> b = std::move(m_free.back());
            m_free.pop_back();
            return b;
        }

        // Dropped when the encoder is too far behind (counted, logged at the end).
        void push(Picture &&p)
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (m_queue.size() >= 8u)
                {
                    ++m_dropped;
                    m_free.push_back(std::move(p.rgba));
                    return;
                }
                m_queue.push_back(std::move(p));
            }
            m_cv.notify_one();
        }

        void close()
        {
            if (m_thread.joinable())
            {
                {
                    std::lock_guard<std::mutex> lock(m_mutex);
                    m_stop = true;
                }
                m_cv.notify_one();
                m_thread.join();
            }
            if (m_codec)
                encode(nullptr);
            if (m_format && m_format->pb)
            {
                av_write_trailer(m_format);
                avio_closep(&m_format->pb);
            }
            if (m_csv)
                std::fclose(m_csv);
            m_csv = nullptr;
            if (m_codec)
                std::fprintf(stderr, "[record] done: %llu frames written, %llu dropped (encoder behind)\n",
                             static_cast<unsigned long long>(m_written), static_cast<unsigned long long>(m_dropped));
            sws_freeContext(m_sws);
            m_sws = nullptr;
            av_frame_free(&m_frame);
            av_packet_free(&m_packet);
            avcodec_free_context(&m_codec);
            avformat_free_context(m_format);
            m_format = nullptr;
        }

    private:
        bool openCodec(const char *name, AVPixelFormat format)
        {
            const AVCodec *codec = avcodec_find_encoder_by_name(name);
            if (!codec)
                return false;
            AVCodecContext *c = avcodec_alloc_context3(codec);
            c->width = m_width;
            c->height = m_height;
            c->pix_fmt = format;
            c->time_base = AVRational{1, 1000}; // milliseconds of the host clock
            c->framerate = AVRational{60, 1};
            c->gop_size = 60;
            c->max_b_frames = 0;
            c->bit_rate = static_cast<int64_t>(m_width) * m_height * 60 / 4; // ~10 Mbit/s at 960x720
            c->colorspace = AVCOL_SPC_SMPTE170M;
            c->color_primaries = AVCOL_PRI_SMPTE170M;
            c->color_trc = AVCOL_TRC_SMPTE170M;
            c->color_range = AVCOL_RANGE_MPEG;
            if (m_format->oformat->flags & AVFMT_GLOBALHEADER)
                c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
            if (std::strstr(name, "nvenc"))
                av_opt_set(c->priv_data, "preset", "p4", 0);
            if (std::strstr(name, "_mf"))
                av_opt_set_int(c->priv_data, "hw_encoding", 1, 0); // the GPU's encoder, not Windows' software one
            if (avcodec_open2(c, codec, nullptr) < 0)
            {
                avcodec_free_context(&c);
                return false;
            }
            m_codec = c;
            return true;
        }

        void encode(AVFrame *frame)
        {
            if (avcodec_send_frame(m_codec, frame) < 0)
                return;
            while (avcodec_receive_packet(m_codec, m_packet) == 0)
            {
                av_packet_rescale_ts(m_packet, m_codec->time_base, m_stream->time_base);
                m_packet->stream_index = m_stream->index;
                av_interleaved_write_frame(m_format, m_packet);
            }
        }

        void run()
        {
            for (;;)
            {
                Picture p;
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_cv.wait(lock, [this] { return m_stop || !m_queue.empty(); });
                    if (m_queue.empty())
                        return;
                    p = std::move(m_queue.front());
                    m_queue.pop_front();
                }
                if (av_frame_make_writable(m_frame) >= 0)
                {
                    const uint8_t *src[1] = {p.rgba.data()};
                    const int srcStride[1] = {m_width * 4};
                    sws_scale(m_sws, src, srcStride, 0, m_height, m_frame->data, m_frame->linesize);
                    // Times strictly increase (two frames in one millisecond: +1).
                    m_frame->pts = std::max<int64_t>(p.ms, m_lastPts + 1);
                    m_lastPts = m_frame->pts;
                    encode(m_frame);
                    if (m_csv)
                        std::fprintf(m_csv, "%llu,%lld,%llu\n", static_cast<unsigned long long>(m_written), static_cast<long long>(m_frame->pts),
                                     static_cast<unsigned long long>(p.tick));
                    ++m_written;
                }
                std::lock_guard<std::mutex> lock(m_mutex);
                m_free.push_back(std::move(p.rgba));
            }
        }

        AVFormatContext *m_format = nullptr;
        AVCodecContext *m_codec = nullptr;
        AVStream *m_stream = nullptr;
        SwsContext *m_sws = nullptr;
        AVFrame *m_frame = nullptr;
        AVPacket *m_packet = nullptr;
        int m_width = 0, m_height = 0;
        std::thread m_thread;
        std::mutex m_mutex;
        std::condition_variable m_cv;
        std::deque<Picture> m_queue;
        std::vector<std::vector<uint8_t>> m_free;
        bool m_stop = false;
        uint64_t m_dropped = 0, m_written = 0;
        int64_t m_lastPts = -1;
        FILE *m_csv = nullptr;
    };

    // The OpenGL side: the frame scaled into a texture of the video's size,
    // read into one of four pixel buffers, collected when its fence passes.
    struct Recorder
    {
        bool failed = false, open = false;
        Encoder encoder;
        int width = 0, height = 0;
        GLuint srcFbo = 0, dstFbo = 0, dstTex = 0;
        struct Slot
        {
            GLuint pbo = 0;
            GLsync fence = nullptr;
            int64_t ms = 0;
            uint64_t tick = 0;
        } slots[4];
        int next = 0;
        std::chrono::steady_clock::time_point start;

        void collect(Slot &s, bool wait)
        {
            if (!s.fence)
                return;
            const GLenum state = glClientWaitSync(s.fence, wait ? GL_SYNC_FLUSH_COMMANDS_BIT : 0, wait ? 1000000000ull : 0ull);
            if (state != GL_ALREADY_SIGNALED && state != GL_CONDITION_SATISFIED)
                return;
            glDeleteSync(s.fence);
            s.fence = nullptr;
            GLint packBuffer = 0;
            glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, s.pbo);
            const size_t bytes = static_cast<size_t>(width) * height * 4u;
            if (const void *mapped = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(bytes), GL_MAP_READ_BIT))
            {
                Picture p;
                p.rgba = encoder.takeBuffer();
                std::memcpy(p.rgba.data(), mapped, bytes);
                p.ms = s.ms;
                p.tick = s.tick;
                encoder.push(std::move(p));
                glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            }
            glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(packBuffer));
        }

        // Oldest first, so frames reach the encoder in order.
        void collectReady(bool wait)
        {
            for (int i = 0; i < 4; ++i)
            {
                Slot &s = slots[(next + i) % 4];
                const bool had = s.fence != nullptr;
                collect(s, wait);
                if (had && s.fence)
                    return; // not ready: the later ones wait for it
            }
        }

        bool begin()
        {
            height = 720;
            width = 960;
            if (const char *v = std::getenv("GT3_RECORD_HEIGHT"); v && *v)
            {
                height = std::clamp(std::atoi(v), 144, 2160) & ~1;
                width = (height * 4 / 3 + 1) & ~1;
            }
            if (!encoder.open(recordPath(), width, height))
                return false;
            glGenFramebuffers(1, &srcFbo);
            glGenFramebuffers(1, &dstFbo);
            glGenTextures(1, &dstTex);
            GLint boundTex = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &boundTex);
            glBindTexture(GL_TEXTURE_2D, dstTex);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, width, height);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(boundTex));
            for (Slot &s : slots)
            {
                glGenBuffers(1, &s.pbo);
                GLint packBuffer = 0;
                glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
                glBindBuffer(GL_PIXEL_PACK_BUFFER, s.pbo);
                glBufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(width) * height * 4, nullptr, GL_STREAM_READ);
                glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(packBuffer));
            }
            start = std::chrono::steady_clock::now();
            return true;
        }

        void capture(GLuint texture, uint32_t w, uint32_t h, uint64_t tick)
        {
            if (failed || w == 0u || h == 0u)
                return;
            if (!open)
            {
                open = begin();
                failed = !open;
                if (failed)
                    return;
            }
            collectReady(false);
            Slot &s = slots[next];
            if (s.fence)
                collect(s, true); // all four still in flight: wait for the oldest
            next = (next + 1) % 4;

            GLint readFbo = 0, drawFbo = 0, packBuffer = 0;
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
            glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
            const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
            if (scissor)
                glDisable(GL_SCISSOR_TEST);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, srcFbo);
            glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, dstFbo);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dstTex, 0);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            glBlitFramebuffer(0, 0, static_cast<GLint>(w), static_cast<GLint>(h), 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, dstFbo);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, s.pbo);
            glPixelStorei(GL_PACK_ALIGNMENT, 4);
            glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
            s.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            s.ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            s.tick = tick;
            glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(packBuffer));
            glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFbo));
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
            if (scissor)
                glEnable(GL_SCISSOR_TEST);
        }

        void finish()
        {
            if (!open)
                return;
            for (int i = 0; i < 4; ++i)
                collect(slots[(next + i) % 4], true);
            encoder.close();
            for (Slot &s : slots)
                glDeleteBuffers(1, &s.pbo);
            glDeleteFramebuffers(1, &srcFbo);
            glDeleteFramebuffers(1, &dstFbo);
            glDeleteTextures(1, &dstTex);
            open = false;
            failed = true; // no second file
        }
    };

    Recorder &recorder()
    {
        static Recorder r;
        return r;
    }
#endif
}

bool gt3rec::enabled()
{
#if PS2X_HAS_FFMPEG
    return recordPath() != nullptr;
#else
    return false;
#endif
}

void gt3rec::captureFrame(unsigned texture, uint32_t width, uint32_t height, uint64_t vsyncTick)
{
#if PS2X_HAS_FFMPEG
    if (recordPath())
        recorder().capture(texture, width, height, vsyncTick);
#else
    (void)texture;
    (void)width;
    (void)height;
    (void)vsyncTick;
#endif
}

void gt3rec::shutdown()
{
#if PS2X_HAS_FFMPEG
    if (recordPath())
        recorder().finish();
#endif
}
