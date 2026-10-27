#include "present_motion.h"

#include "rlgl.h"
#include "external/glad.h"

#include <algorithm>
#include <cmath>

namespace
{
    // Frame pixels (640 x 448, row 0 at the top) to clip space with row 0 at
    // texture row 0, as the other presentation passes (no flip).
    const char *const kBuildVertex = R"GLSL(#version 430
layout(location = 0) in vec2 a_pos;    // frame pixels
layout(location = 1) in float a_q;     // 1/w, larger is nearer
layout(location = 2) in vec2 a_motion; // previous position - current, frame pixels
out vec2 v_motion;
void main()
{
    v_motion = a_motion;
    gl_Position = vec4(a_pos.x / 320.0 - 1.0, a_pos.y / 224.0 - 1.0, 1.0 / (1.0 + max(a_q, 0.0)) * 2.0 - 1.0, 1.0);
}
)GLSL";
    const char *const kBuildFragment = R"GLSL(#version 430
in vec2 v_motion;
layout(location = 0) out vec2 o_motion;
void main() { o_motion = v_motion; }
)GLSL";

    // raylib's names (texture0, fragTexCoord); the motion texture's row 0 is
    // the picture's top like the frame texture.
    const char *const kDebugFragment = R"GLSL(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;
out vec4 finalColor;
vec3 hsv(float h, float s, float v)
{
    vec3 k = clamp(abs(mod(h * 6.0 + vec3(0.0, 4.0, 2.0), 6.0) - 3.0) - 1.0, 0.0, 1.0);
    return v * mix(vec3(1.0), k, s);
}
void main()
{
    vec2 m = texture(texture0, fragTexCoord).rg;
    float len = length(m);
    if (len < 0.05)
        discard;
    float h = atan(m.y, m.x) / 6.2831853 + 0.5;
    finalColor = vec4(hsv(h, 0.9, clamp(0.35 + len / 6.0, 0.0, 1.0)), 0.75);
}
)GLSL";

    GLuint compile(GLenum type, const char *source, std::string &error)
    {
        const GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint ok = 0;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[2048];
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            error = log;
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }
}

bool MotionBuffer::init(std::string &error)
{
    const GLuint vs = compile(GL_VERTEX_SHADER, kBuildVertex, error);
    const GLuint fs = vs ? compile(GL_FRAGMENT_SHADER, kBuildFragment, error) : 0;
    if (!vs || !fs)
        return false;
    m_program = glCreateProgram();
    glAttachShader(m_program, vs);
    glAttachShader(m_program, fs);
    glLinkProgram(m_program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(m_program, GL_LINK_STATUS, &linked);
    if (!linked)
    {
        error = "motion program link failed";
        return false;
    }
    glGenVertexArrays(1, &m_vao);
    glGenBuffers(1, &m_vbo);
    glBindVertexArray(m_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void *>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void *>(2 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), reinterpret_cast<void *>(3 * sizeof(float)));
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    m_debug = LoadShaderFromMemory(nullptr, kDebugFragment);
    return true;
}

void MotionBuffer::shutdown()
{
    if (m_tex)
        glDeleteTextures(1, &m_tex);
    if (m_depth)
        glDeleteRenderbuffers(1, &m_depth);
    if (m_fbo)
        glDeleteFramebuffers(1, &m_fbo);
    if (m_vbo)
        glDeleteBuffers(1, &m_vbo);
    if (m_vao)
        glDeleteVertexArrays(1, &m_vao);
    if (m_program)
        glDeleteProgram(m_program);
    if (m_debug.id)
        UnloadShader(m_debug);
    *this = MotionBuffer{};
}

bool MotionBuffer::ensure(int width, int height)
{
    if (m_tex && width == m_width && height == m_height)
        return true;
    if (m_tex)
        glDeleteTextures(1, &m_tex);
    if (m_depth)
        glDeleteRenderbuffers(1, &m_depth);
    if (!m_fbo)
        glGenFramebuffers(1, &m_fbo);
    glGenTextures(1, &m_tex);
    glBindTexture(GL_TEXTURE_2D, m_tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RG16F, width, height);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    glGenRenderbuffers(1, &m_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, m_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, width, height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_depth);
    const bool complete = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    m_width = width;
    m_height = height;
    return complete;
}

void MotionBuffer::build(const std::vector<gt3motion::Triangle> &triangles, int width, int height)
{
    if (!m_program || width <= 0 || height <= 0 || !ensure(width, height))
        return;
    m_vertices.clear();
    m_vertices.reserve(triangles.size() * 15u);
    for (const gt3motion::Triangle &t : triangles)
        for (const gt3motion::Vertex &v : t.v)
        {
            m_vertices.push_back(v.x);
            m_vertices.push_back(v.y);
            m_vertices.push_back(v.q);
            m_vertices.push_back(v.px - v.x);
            m_vertices.push_back(v.py - v.y);
        }
    rlDrawRenderBatchActive();
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glViewport(0, 0, width, height);
    const GLfloat zero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    glClearBufferfv(GL_COLOR, 0, zero);
    const GLfloat far = 1.0f;
    glClearBufferfv(GL_DEPTH, 0, &far);
    if (!m_vertices.empty())
    {
        glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
        glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(m_vertices.size() * sizeof(float)), m_vertices.data(), GL_STREAM_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glUseProgram(m_program);
        glBindVertexArray(m_vao);
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(m_vertices.size() / 5u));
        glBindVertexArray(0);
        glUseProgram(0);
    }
    // raylib's state (rlglInit): no depth test, LEQUAL, blending and back-face
    // culling on; it does not set them again.
    glDisable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, GetRenderWidth(), GetRenderHeight());
}

void MotionBuffer::drawDebug(const Rectangle &dst)
{
    if (!m_tex || !m_debug.id)
        return;
    const Texture2D t{m_tex, m_width, m_height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    BeginShaderMode(m_debug);
    DrawTexturePro(t, Rectangle{0.0f, 0.0f, static_cast<float>(m_width), static_cast<float>(m_height)}, dst, Vector2{0.0f, 0.0f}, 0.0f, WHITE);
    EndShaderMode();
}

namespace
{
    // Addressed by gl_FragCoord in texels of the frame texture (row 0 = the
    // picture's top in every texture, as the other presentation passes).
    const char *const kGenerateFragment = R"GLSL(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;  // the newer frame (phase 1)
uniform sampler2D texPrev;   // the older frame (phase 0)
uniform sampler2D texMotion; // the newer picture's motion: previous - current, 640 x 448 frame pixels
uniform float phase;
uniform vec2 texSize;        // frame texture, texels
uniform vec2 picSize;        // the picture at its top-left, texels
uniform sampler2D texOverlay; // overlay mask (updateOverlay), picture texels
out vec4 finalColor;
vec2 motionAt(vec2 texel) // in texels
{
    vec2 m = texture(texMotion, texel / picSize).rg;
    return m * (picSize / vec2(640.0, 448.0));
}
void main()
{
    vec2 p = gl_FragCoord.xy;
    if (p.x >= picSize.x || p.y >= picSize.y)
    {
        finalColor = texture(texture0, p / texSize);
        return;
    }
    if (texture(texOverlay, p / picSize).r > 0.5)
    {
        // An overlay (HUD): as it is.
        finalColor = vec4(mix(texture(texPrev, p / texSize).rgb, texture(texture0, p / texSize).rgb, phase), 1.0);
        return;
    }
    // Where this pixel's surface is in the newer frame: c + (1 - phase) * m(c) = p.
    float back = 1.0 - phase;
    vec2 m = motionAt(p);
    vec2 c = p - back * m;
    m = motionAt(c);
    c = p - back * m;
    vec4 cur = texture(texture0, c / texSize);
    vec4 old = texture(texPrev, (c + m) / texSize);
    vec4 curHere = texture(texture0, p / texSize);
    vec4 oldHere = texture(texPrev, p / texSize);
    float errMoving = dot(abs(cur.rgb - old.rgb), vec3(1.0));
    float errStill = dot(abs(curHere.rgb - oldHere.rgb), vec3(1.0));
    vec3 color = errStill <= errMoving ? mix(oldHere.rgb, curHere.rgb, phase) : mix(old.rgb, cur.rgb, phase);
    finalColor = vec4(color, 1.0);
}
)GLSL";
}

namespace
{
    // The overlay mask, per new game frame: 1 where the pixel kept its colour
    // although the scene there moves (at least 0.75 frame pixels), 0 where it
    // changed; otherwise as before. Held with a quick rise and a quick fall.
    const char *const kOverlayFragment = R"GLSL(#version 330
in vec2 fragTexCoord;
in vec4 fragColor;
uniform sampler2D texture0;  // the newer frame
uniform sampler2D texPrev;   // the older frame
uniform sampler2D texMotion; // the newer picture's motion (frame pixels)
uniform sampler2D texOld;    // the mask so far (picture texels)
uniform vec2 texSize;
uniform vec2 picSize;
out vec4 finalColor;
void main()
{
    vec2 p = gl_FragCoord.xy;
    float old = texture(texOld, p / picSize).r;
    vec3 a = texture(texture0, p / texSize).rgb, b = texture(texPrev, p / texSize).rgb;
    float change = dot(abs(a - b), vec3(1.0));
    float moving = length(texture(texMotion, p / picSize).rg);
    float mask = old;
    if (change > 0.06)
        mask = old * 0.25;
    else if (moving > 0.75)
        mask = min(1.0, old + 0.34);
    finalColor = vec4(mask, 0.0, 0.0, 1.0);
}
)GLSL";
}

void FrameGenerator::updateOverlay(const Texture2D &prev, const Texture2D &cur, unsigned motion, int picWidth, int picHeight)
{
    if (!m_overlayShader.id)
        return;
    for (RenderTexture2D &t : m_overlay)
        if (!t.id || t.texture.width != picWidth || t.texture.height != picHeight)
        {
            if (t.id)
                UnloadRenderTexture(t);
            t = LoadRenderTexture(picWidth, picHeight);
            SetTextureFilter(t.texture, TEXTURE_FILTER_POINT);
            BeginTextureMode(t);
            ClearBackground(BLACK);
            EndTextureMode();
        }
    RenderTexture2D &from = m_overlay[m_overlayCur], &to = m_overlay[m_overlayCur ^ 1];
    const float texSize[2] = {static_cast<float>(cur.width), static_cast<float>(cur.height)};
    const float picSize[2] = {static_cast<float>(picWidth), static_cast<float>(picHeight)};
    const Texture2D motionTex{motion, picWidth, picHeight, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    BeginTextureMode(to);
    BeginShaderMode(m_overlayShader);
    SetShaderValueTexture(m_overlayShader, m_oLocPrev, prev);
    SetShaderValueTexture(m_overlayShader, m_oLocMotion, motionTex);
    SetShaderValueTexture(m_overlayShader, m_oLocOld, from.texture);
    SetShaderValue(m_overlayShader, m_oLocTexSize, texSize, SHADER_UNIFORM_VEC2);
    SetShaderValue(m_overlayShader, m_oLocPicSize, picSize, SHADER_UNIFORM_VEC2);
    DrawTexturePro(cur, Rectangle{0.0f, 0.0f, picSize[0], picSize[1]}, Rectangle{0.0f, 0.0f, picSize[0], picSize[1]}, Vector2{0.0f, 0.0f}, 0.0f,
                   WHITE);
    EndShaderMode();
    EndTextureMode();
    m_overlayCur ^= 1;
}

bool FrameGenerator::init(std::string &error)
{
    m_shader = LoadShaderFromMemory(nullptr, kGenerateFragment);
    if (!m_shader.id || m_shader.id == rlGetShaderIdDefault())
    {
        error = "frame generation shader failed";
        return false;
    }
    m_locPrev = GetShaderLocation(m_shader, "texPrev");
    m_locMotion = GetShaderLocation(m_shader, "texMotion");
    m_locPhase = GetShaderLocation(m_shader, "phase");
    m_locTexSize = GetShaderLocation(m_shader, "texSize");
    m_locPicSize = GetShaderLocation(m_shader, "picSize");
    m_locOverlay = GetShaderLocation(m_shader, "texOverlay");
    m_overlayShader = LoadShaderFromMemory(nullptr, kOverlayFragment);
    m_oLocPrev = GetShaderLocation(m_overlayShader, "texPrev");
    m_oLocMotion = GetShaderLocation(m_overlayShader, "texMotion");
    m_oLocOld = GetShaderLocation(m_overlayShader, "texOld");
    m_oLocTexSize = GetShaderLocation(m_overlayShader, "texSize");
    m_oLocPicSize = GetShaderLocation(m_overlayShader, "picSize");
    return true;
}

void FrameGenerator::shutdown()
{
    if (m_target.id)
        UnloadRenderTexture(m_target);
    for (RenderTexture2D &t : m_overlay)
        if (t.id)
            UnloadRenderTexture(t);
    if (m_shader.id)
        UnloadShader(m_shader);
    if (m_overlayShader.id)
        UnloadShader(m_overlayShader);
    *this = FrameGenerator{};
}

const Texture2D &FrameGenerator::generate(const Texture2D &prev, const Texture2D &cur, unsigned motion, int picWidth, int picHeight, float phase)
{
    if (!m_target.id || m_target.texture.width != cur.width || m_target.texture.height != cur.height)
    {
        if (m_target.id)
            UnloadRenderTexture(m_target);
        m_target = LoadRenderTexture(cur.width, cur.height);
        SetTextureFilter(m_target.texture, TEXTURE_FILTER_BILINEAR);
    }
    const float texSize[2] = {static_cast<float>(cur.width), static_cast<float>(cur.height)};
    const float picSize[2] = {static_cast<float>(picWidth), static_cast<float>(picHeight)};
    const Texture2D motionTex{motion, picWidth, picHeight, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
    BeginTextureMode(m_target);
    BeginShaderMode(m_shader);
    SetShaderValueTexture(m_shader, m_locPrev, prev);
    SetShaderValueTexture(m_shader, m_locMotion, motionTex);
    SetShaderValue(m_shader, m_locPhase, &phase, SHADER_UNIFORM_FLOAT);
    SetShaderValue(m_shader, m_locTexSize, texSize, SHADER_UNIFORM_VEC2);
    SetShaderValue(m_shader, m_locPicSize, picSize, SHADER_UNIFORM_VEC2);
    if (m_overlay[m_overlayCur].id)
        SetShaderValueTexture(m_shader, m_locOverlay, m_overlay[m_overlayCur].texture);
    DrawTexturePro(cur, Rectangle{0.0f, 0.0f, texSize[0], texSize[1]}, Rectangle{0.0f, 0.0f, texSize[0], texSize[1]}, Vector2{0.0f, 0.0f}, 0.0f,
                   WHITE);
    EndShaderMode();
    EndTextureMode();
    return m_target.texture;
}
