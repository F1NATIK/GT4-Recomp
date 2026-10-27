#include "gt3_phone_view.h"

#include "external/glad.h"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace phoneview
{
    namespace
    {
        // The mirror enlarged on the GPU before it is read back (a small
        // picture at native scale looks soft once a phone stretches it): a
        // sharp cubic (Keys, a = -0.6) with taps clamped to the mirror's own
        // rectangle, rows kept in the source's order.
        const char kUpscaleVs[] = R"GLSL(#version 330
void main()
{
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";
        const char kUpscaleFs[] = R"GLSL(#version 330
uniform sampler2D u_src;
uniform vec4 u_rect; // x, y, w, h in texels
uniform float u_k;   // output pixels per source pixel
out vec4 o_colour;
float keys(float x)
{
    const float a = -0.6;
    x = abs(x);
    if (x <= 1.0)
        return ((a + 2.0) * x - (a + 3.0)) * x * x + 1.0;
    if (x < 2.0)
        return ((a * x - 5.0 * a) * x + 8.0 * a) * x - 4.0 * a;
    return 0.0;
}
void main()
{
    vec2 pos = u_rect.xy + gl_FragCoord.xy / u_k;
    vec2 base = floor(pos - 0.5);
    vec2 f = pos - 0.5 - base;
    vec2 lo = u_rect.xy, hi = u_rect.xy + u_rect.zw - 1.0;
    vec4 sum = vec4(0.0);
    float weight = 0.0;
    for (int j = -1; j <= 2; ++j)
    {
        float wy = keys(float(j) - f.y);
        for (int i = -1; i <= 2; ++i)
        {
            float w = keys(float(i) - f.x) * wy;
            sum += texelFetch(u_src, ivec2(clamp(base + vec2(i, j), lo, hi)), 0) * w;
            weight += w;
        }
    }
    o_colour = vec4(clamp(sum.rgb / weight, 0.0, 1.0), 1.0);
}
)GLSL";

        GLuint compile(GLenum type, const char *source)
        {
            const GLuint s = glCreateShader(type);
            glShaderSource(s, 1, &source, nullptr);
            glCompileShader(s);
            GLint ok = 0;
            glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
            if (!ok)
            {
                char log[1024] = {};
                glGetShaderInfoLog(s, sizeof(log), nullptr, log);
                std::cerr << "[phone-view] upscale shader: " << log << std::endl;
                glDeleteShader(s);
                return 0;
            }
            return s;
        }

        // GT3_PHONE_VIEW_WIDTH=<pixels> (default 960): the mirror is enlarged
        // by a whole factor (at most 4) until it is at least this wide; 0 off.
        int targetWidth()
        {
            static const int width = [] {
                const char *v = std::getenv("GT3_PHONE_VIEW_WIDTH");
                return v && *v ? std::clamp(std::atoi(v), 0, 4096) : 960;
            }();
            return width;
        }
    }

    Capture::~Capture()
    {
        for (Slot &s : m_slots)
        {
            if (s.fence)
                glDeleteSync(static_cast<GLsync>(s.fence));
            if (s.buffer)
                glDeleteBuffers(1, &s.buffer);
        }
        if (m_fbo)
            glDeleteFramebuffers(1, &m_fbo);
        if (m_drawFbo)
            glDeleteFramebuffers(1, &m_drawFbo);
        if (m_big)
            glDeleteTextures(1, &m_big);
        if (m_vao)
            glDeleteVertexArrays(1, &m_vao);
        if (m_program > 0)
            glDeleteProgram(m_program);
    }

    // Draws texels x, y, w, h of `texture` k times larger into m_big
    // (k * w by k * h); false if the program is not available.
    bool Capture::enlarge(unsigned texture, int x, int y, int w, int h, int k)
    {
        if (m_program == 0)
        {
            m_program = -1;
            const GLuint vs = compile(GL_VERTEX_SHADER, kUpscaleVs), fs = compile(GL_FRAGMENT_SHADER, kUpscaleFs);
            if (vs && fs)
            {
                const GLuint p = glCreateProgram();
                glAttachShader(p, vs);
                glAttachShader(p, fs);
                glLinkProgram(p);
                GLint ok = 0;
                glGetProgramiv(p, GL_LINK_STATUS, &ok);
                if (ok)
                {
                    m_program = static_cast<int>(p);
                    m_rectLoc = glGetUniformLocation(p, "u_rect");
                    m_scaleLoc = glGetUniformLocation(p, "u_k");
                    glProgramUniform1i(p, glGetUniformLocation(p, "u_src"), 0);
                }
                else
                    glDeleteProgram(p);
            }
            if (vs)
                glDeleteShader(vs);
            if (fs)
                glDeleteShader(fs);
            if (m_program < 0)
                std::cerr << "[phone-view] mirror upscaling unavailable" << std::endl;
        }
        if (m_program < 0)
            return false;
        const int bw = w * k, bh = h * k;
        if (!m_big || m_bigW != bw || m_bigH != bh)
        {
            if (m_big)
                glDeleteTextures(1, &m_big);
            glGenTextures(1, &m_big);
            GLint bound = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);
            glBindTexture(GL_TEXTURE_2D, m_big);
            glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, bw, bh);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound));
            m_bigW = bw;
            m_bigH = bh;
            if (!m_drawFbo)
                glGenFramebuffers(1, &m_drawFbo);
            if (!m_vao)
                glGenVertexArrays(1, &m_vao);
        }
        // The present loop's state, put back afterwards.
        GLint drawFbo = 0, program = 0, vao = 0, activeTexture = 0, bound = 0, viewport[4] = {};
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFbo);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        glGetIntegerv(GL_VIEWPORT, viewport);
        const GLboolean blend = glIsEnabled(GL_BLEND), depth = glIsEnabled(GL_DEPTH_TEST), scissor = glIsEnabled(GL_SCISSOR_TEST),
                        cull = glIsEnabled(GL_CULL_FACE);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound);

        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_drawFbo);
        glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_big, 0);
        glViewport(0, 0, bw, bh);
        glDisable(GL_BLEND);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_CULL_FACE);
        glUseProgram(static_cast<GLuint>(m_program));
        glUniform4f(m_rectLoc, static_cast<float>(x), static_cast<float>(y), static_cast<float>(w), static_cast<float>(h));
        glUniform1f(m_scaleLoc, static_cast<float>(k));
        glBindTexture(GL_TEXTURE_2D, texture);
        glBindVertexArray(m_vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        glBindVertexArray(static_cast<GLuint>(vao));
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(bound));
        glActiveTexture(static_cast<GLenum>(activeTexture));
        glUseProgram(static_cast<GLuint>(program));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(drawFbo));
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        if (blend)
            glEnable(GL_BLEND);
        if (depth)
            glEnable(GL_DEPTH_TEST);
        if (scissor)
            glEnable(GL_SCISSOR_TEST);
        if (cull)
            glEnable(GL_CULL_FACE);
        return true;
    }

    void Capture::read(unsigned texture, int x, int y, int w, int h)
    {
        if (w <= 0 || h <= 0)
            return;
        Slot &s = m_slots[m_next];
        if (s.busy)
            return; // all readbacks still in flight: skip this picture
        const int k = targetWidth() > 0 ? std::clamp((targetWidth() + w - 1) / w, 1, 4) : 1;
        if (k > 1 && enlarge(texture, x, y, w, h, k))
        {
            texture = m_big;
            x = y = 0;
            w *= k;
            h *= k;
        }
        m_next = (m_next + 1) % 3;
        if (!m_fbo)
            glGenFramebuffers(1, &m_fbo);
        if (!s.buffer)
            glGenBuffers(1, &s.buffer);
        GLint readFbo = 0, packBuffer = 0;
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFbo);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo);
        glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, s.buffer);
        const GLsizeiptr bytes = static_cast<GLsizeiptr>(w) * h * 4;
        glBufferData(GL_PIXEL_PACK_BUFFER, bytes, nullptr, GL_STREAM_READ);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        s.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        s.w = w;
        s.h = h;
        s.busy = true;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(packBuffer));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(readFbo));
    }

    void Capture::poll()
    {
        for (Slot &s : m_slots)
        {
            if (!s.busy)
                continue;
            const GLenum state = glClientWaitSync(static_cast<GLsync>(s.fence), 0, 0);
            if (state != GL_ALREADY_SIGNALED && state != GL_CONDITION_SATISFIED)
                continue;
            glDeleteSync(static_cast<GLsync>(s.fence));
            s.fence = nullptr;
            s.busy = false;
            GLint packBuffer = 0;
            glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &packBuffer);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, s.buffer);
            const auto *pixels = static_cast<const uint8_t *>(
                glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, static_cast<GLsizeiptr>(s.w) * s.h * 4, GL_MAP_READ_BIT));
            if (pixels)
            {
                submit(pixels, s.w, s.h);
                glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
            }
            glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(packBuffer));
        }
    }
}
