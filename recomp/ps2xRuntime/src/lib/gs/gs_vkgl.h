#pragma once

// vkgl: the subset of OpenGL the GS renderer (gs_gl_backend.cpp) uses,
// implemented on Vulkan (gs_vkgl.cpp). gs_vk_backend.cpp compiles the OpenGL
// renderer's source a second time with its gl* calls redirected here
// (VKGL_REDIRECT), so both renderers share one source: page ownership,
// batching, shaders and all.
//
// Scope: one thread (the GS renderer thread), one context. Objects follow
// OpenGL's names and state (texture units, image units, shader storage
// bindings, framebuffers with colour/depth attachments, programs with loose
// uniforms). The Vulkan side is a state tracker: render passes begin when a
// draw needs one and end at anything else (copies, uploads, read-backs,
// dispatches, barriers); pipelines come from a cache keyed by the state; every
// image stays in the GENERAL layout and hazards get global memory barriers.

#include "external/glad.h"

#include <cstdint>
#include <vector>

#ifdef MemoryBarrier
#undef MemoryBarrier // winnt.h's macro; here it is glMemoryBarrier
#endif

namespace vkgl
{
    // False when Vulkan (1.3 with dynamic rendering) is not usable; then no
    // other vkgl call may be made. Needs the OpenGL context current (the
    // device is picked to match its GPU).
    bool available();

    // GT4's upscalers (gt4_upscale.h; DLSS): a texture's Vulkan image (handles as integers; every vkgl image is in
    // the GENERAL layout), and outside work recorded into the renderer's command buffer: the render pass ends, the
    // listed textures are ordered after earlier work (reads, writes) and later work after them (writes), fn records
    // into the command buffer (a VkCommandBuffer), and the renderer forgets its bound pipeline, descriptors and
    // constants (the outside work binds its own).
    struct NativeImage
    {
        uint64_t image = 0, view = 0; // VkImage, VkImageView
        uint32_t format = 0, usage = 0; // VkFormat, VkImageUsageFlags
        int width = 0, height = 0;
    };
    bool nativeImage(GLuint texture, NativeImage &out);
    void recordExternal(const GLuint *reads, int readCount, const GLuint *writes, int writeCount, void (*fn)(void *cmd, void *user),
                        void *user);
    // Only inside recordExternal's fn, for outside work on another API (FSR 4 through Direct3D 12): what is recorded so far
    // is submitted, signalling the timeline semaphore `semaphore` (a VkSemaphore) to signalValue; the commands after it
    // wait for waitValue on it. Returns the new command buffer (a VkCommandBuffer) to record the rest into.
    void *splitExternal(uint64_t semaphore, uint64_t signalValue, uint64_t waitValue);
    // GT4's mirror page (gt4_mirror.h): a rectangle of a 4-byte texture read back without waiting for the GPU. readStart
    // records the copy into one of a few host buffers (false: none free, or not such a texture); readTake hands over the
    // oldest finished one (rows of w texels, the texture's row y first; tag as given), false while none has finished.
    bool readStart(GLuint texture, int x, int y, int w, int h, uint32_t tag);
    bool readTake(std::vector<uint8_t> &out, int &w, int &h, uint32_t &tag);

    void GLAD_API_PTR ActiveTexture(GLenum texture);
    void GLAD_API_PTR AttachShader(GLuint program, GLuint shader);
    void GLAD_API_PTR BeginQuery(GLenum target, GLuint id);
    void GLAD_API_PTR BindBuffer(GLenum target, GLuint buffer);
    void GLAD_API_PTR BindBufferBase(GLenum target, GLuint index, GLuint buffer);
    void GLAD_API_PTR BindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset, GLsizeiptr size);
    void GLAD_API_PTR BindFramebuffer(GLenum target, GLuint framebuffer);
    void GLAD_API_PTR BindImageTexture(GLuint unit, GLuint texture, GLint level, GLboolean layered, GLint layer, GLenum access, GLenum format);
    void GLAD_API_PTR BindTexture(GLenum target, GLuint texture);
    void GLAD_API_PTR BindVertexArray(GLuint array);
    void GLAD_API_PTR BlendEquationSeparate(GLenum modeRGB, GLenum modeAlpha);
    void GLAD_API_PTR BlendFuncSeparate(GLenum srcRGB, GLenum dstRGB, GLenum srcAlpha, GLenum dstAlpha);
    void GLAD_API_PTR BlitFramebuffer(GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1,
                                      GLbitfield mask, GLenum filter);
    void GLAD_API_PTR BufferData(GLenum target, GLsizeiptr size, const void *data, GLenum usage);
    void GLAD_API_PTR BufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void *data);
    GLenum GLAD_API_PTR ClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout);
    void GLAD_API_PTR ColorMask(GLboolean red, GLboolean green, GLboolean blue, GLboolean alpha);
    void GLAD_API_PTR CompileShader(GLuint shader);
    void GLAD_API_PTR CopyImageSubData(GLuint srcName, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ, GLuint dstName,
                                       GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ, GLsizei srcWidth, GLsizei srcHeight,
                                       GLsizei srcDepth);
    GLuint GLAD_API_PTR CreateProgram();
    GLuint GLAD_API_PTR CreateShader(GLenum type);
    void GLAD_API_PTR DebugMessageCallback(GLDEBUGPROC callback, const void *userParam);
    // (GT4: the stable targets' per-texture framebuffers are deleted with their texture.)
    void GLAD_API_PTR DeleteFramebuffers(GLsizei n, const GLuint *framebuffers);
    void GLAD_API_PTR DeleteProgram(GLuint program);
    void GLAD_API_PTR DeleteShader(GLuint shader);
    void GLAD_API_PTR DeleteSync(GLsync sync);
    void GLAD_API_PTR DeleteTextures(GLsizei n, const GLuint *textures);
    void GLAD_API_PTR DepthFunc(GLenum func);
    void GLAD_API_PTR DepthMask(GLboolean flag);
    void GLAD_API_PTR Disable(GLenum cap);
    void GLAD_API_PTR DispatchCompute(GLuint x, GLuint y, GLuint z);
    void GLAD_API_PTR DrawArrays(GLenum mode, GLint first, GLsizei count);
    void GLAD_API_PTR DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instances);
    void GLAD_API_PTR DrawBuffer(GLenum buf);
    void GLAD_API_PTR Enable(GLenum cap);
    void GLAD_API_PTR EndQuery(GLenum target);
    GLsync GLAD_API_PTR FenceSync(GLenum condition, GLbitfield flags);
    void GLAD_API_PTR Finish();
    void GLAD_API_PTR Flush();
    void GLAD_API_PTR FramebufferParameteri(GLenum target, GLenum pname, GLint param);
    void GLAD_API_PTR FramebufferTexture2D(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level);
    void GLAD_API_PTR GenBuffers(GLsizei n, GLuint *buffers);
    void GLAD_API_PTR GenFramebuffers(GLsizei n, GLuint *framebuffers);
    void GLAD_API_PTR GenQueries(GLsizei n, GLuint *ids);
    void GLAD_API_PTR GenSamplers(GLsizei n, GLuint *samplers);
    void GLAD_API_PTR GenTextures(GLsizei n, GLuint *textures);
    void GLAD_API_PTR GenVertexArrays(GLsizei n, GLuint *arrays);
    GLenum GLAD_API_PTR GetError();
    void GLAD_API_PTR GetIntegerv(GLenum pname, GLint *data);
    void GLAD_API_PTR GetProgramBinary(GLuint program, GLsizei bufSize, GLsizei *length, GLenum *binaryFormat, void *binary);
    void GLAD_API_PTR GetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
    void GLAD_API_PTR GetProgramiv(GLuint program, GLenum pname, GLint *params);
    void GLAD_API_PTR GetQueryObjectiv(GLuint id, GLenum pname, GLint *params);
    void GLAD_API_PTR GetQueryObjectui64v(GLuint id, GLenum pname, GLuint64 *params);
    void GLAD_API_PTR GetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei *length, GLchar *infoLog);
    void GLAD_API_PTR GetShaderiv(GLuint shader, GLenum pname, GLint *params);
    const GLubyte *GLAD_API_PTR GetString(GLenum name);
    const GLubyte *GLAD_API_PTR GetStringi(GLenum name, GLuint index);
    void GLAD_API_PTR GetTexImage(GLenum target, GLint level, GLenum format, GLenum type, void *pixels);
    GLint GLAD_API_PTR GetUniformLocation(GLuint program, const GLchar *name);
    void GLAD_API_PTR LinkProgram(GLuint program);
    void *GLAD_API_PTR MapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length, GLbitfield access);
    void GLAD_API_PTR MemoryBarrier(GLbitfield barriers);
    void GLAD_API_PTR PixelStorei(GLenum pname, GLint param);
    void GLAD_API_PTR ReadBuffer(GLenum src);
    void GLAD_API_PTR ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void *pixels);
    void GLAD_API_PTR SamplerParameteri(GLuint sampler, GLenum pname, GLint param);
    void GLAD_API_PTR ShaderSource(GLuint shader, GLsizei count, const GLchar *const *string, const GLint *length);
    void GLAD_API_PTR TexParameteri(GLenum target, GLenum pname, GLint param);
    void GLAD_API_PTR TexStorage2D(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height);
    void GLAD_API_PTR TexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format,
                                    GLenum type, const void *pixels);
    void GLAD_API_PTR Uniform1f(GLint location, GLfloat v0);
    void GLAD_API_PTR Uniform1i(GLint location, GLint v0);
    void GLAD_API_PTR Uniform1ui(GLint location, GLuint v0);
    void GLAD_API_PTR Uniform2f(GLint location, GLfloat v0, GLfloat v1);
    void GLAD_API_PTR Uniform2i(GLint location, GLint v0, GLint v1);
    void GLAD_API_PTR Uniform3i(GLint location, GLint v0, GLint v1, GLint v2);
    void GLAD_API_PTR Uniform4i(GLint location, GLint v0, GLint v1, GLint v2, GLint v3);
    void GLAD_API_PTR Uniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3);
    void GLAD_API_PTR Uniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2);
    void GLAD_API_PTR UseProgram(GLuint program);
    void GLAD_API_PTR Viewport(GLint x, GLint y, GLsizei width, GLsizei height);

    // Extension entry points by name (bindless handles, clip control, buffer
    // storage, glClearTexImage); null for what vkgl does not offer (texture
    // barriers without local read).
    void *getProcAddress(const char *name);

    // Presentation (the renderer's gsgl::beginPresentSlot / endPresentSlot in
    // the Vulkan build): the frame is drawn into a vkgl texture whose memory
    // OpenGL shares; presentEnd submits, then on the OpenGL context of the
    // calling thread waits for Vulkan, copies it into the host's presentation
    // slot (gs_gl_present) and publishes it.
    GLuint presentBegin();
    void presentEnd(uint32_t width, uint32_t height);
    // False without OpenGL interop (then frames go through the CPU).
    bool presentAvailable();

    // Whether depth textures can be blitted (VK_FORMAT_D32_SFLOAT blit support).
    bool depthBlitSupported();
    // Pages into scaled targets repeated on the GPU (GT3_VKGL_GPU_UPSAMPLE=0: on the CPU).
    bool gpuUpsampleOn();

    // The renderer's batch samples these textures through bindless handles
    // (its batchTextures), which vkgl cannot see otherwise: its draws' hazards.
    void noteSampledTextures(const GLuint *textures, size_t count);
}

#if defined(VKGL_REDIRECT)
#define VKGL_ROUTE(name) vkgl::name
#undef glActiveTexture
#define glActiveTexture VKGL_ROUTE(ActiveTexture)
#undef glAttachShader
#define glAttachShader VKGL_ROUTE(AttachShader)
#undef glBeginQuery
#define glBeginQuery VKGL_ROUTE(BeginQuery)
#undef glBindBuffer
#define glBindBuffer VKGL_ROUTE(BindBuffer)
#undef glBindBufferBase
#define glBindBufferBase VKGL_ROUTE(BindBufferBase)
#undef glBindBufferRange
#define glBindBufferRange VKGL_ROUTE(BindBufferRange)
#undef glBindFramebuffer
#define glBindFramebuffer VKGL_ROUTE(BindFramebuffer)
#undef glBindImageTexture
#define glBindImageTexture VKGL_ROUTE(BindImageTexture)
#undef glBindTexture
#define glBindTexture VKGL_ROUTE(BindTexture)
#undef glBindVertexArray
#define glBindVertexArray VKGL_ROUTE(BindVertexArray)
#undef glBlendEquationSeparate
#define glBlendEquationSeparate VKGL_ROUTE(BlendEquationSeparate)
#undef glBlendFuncSeparate
#define glBlendFuncSeparate VKGL_ROUTE(BlendFuncSeparate)
#undef glBlitFramebuffer
#define glBlitFramebuffer VKGL_ROUTE(BlitFramebuffer)
#undef glBufferData
#define glBufferData VKGL_ROUTE(BufferData)
#undef glBufferSubData
#define glBufferSubData VKGL_ROUTE(BufferSubData)
#undef glClientWaitSync
#define glClientWaitSync VKGL_ROUTE(ClientWaitSync)
#undef glColorMask
#define glColorMask VKGL_ROUTE(ColorMask)
#undef glCompileShader
#define glCompileShader VKGL_ROUTE(CompileShader)
#undef glCopyImageSubData
#define glCopyImageSubData VKGL_ROUTE(CopyImageSubData)
#undef glCreateProgram
#define glCreateProgram VKGL_ROUTE(CreateProgram)
#undef glCreateShader
#define glCreateShader VKGL_ROUTE(CreateShader)
#undef glDebugMessageCallback
#define glDebugMessageCallback VKGL_ROUTE(DebugMessageCallback)
#undef glDeleteFramebuffers
#define glDeleteFramebuffers VKGL_ROUTE(DeleteFramebuffers)
#undef glDeleteProgram
#define glDeleteProgram VKGL_ROUTE(DeleteProgram)
#undef glDeleteShader
#define glDeleteShader VKGL_ROUTE(DeleteShader)
#undef glDeleteSync
#define glDeleteSync VKGL_ROUTE(DeleteSync)
#undef glDeleteTextures
#define glDeleteTextures VKGL_ROUTE(DeleteTextures)
#undef glDepthFunc
#define glDepthFunc VKGL_ROUTE(DepthFunc)
#undef glDepthMask
#define glDepthMask VKGL_ROUTE(DepthMask)
#undef glDisable
#define glDisable VKGL_ROUTE(Disable)
#undef glDispatchCompute
#define glDispatchCompute VKGL_ROUTE(DispatchCompute)
#undef glDrawArrays
#define glDrawArrays VKGL_ROUTE(DrawArrays)
#undef glDrawArraysInstanced
#define glDrawArraysInstanced VKGL_ROUTE(DrawArraysInstanced)
#undef glDrawBuffer
#define glDrawBuffer VKGL_ROUTE(DrawBuffer)
#undef glEnable
#define glEnable VKGL_ROUTE(Enable)
#undef glEndQuery
#define glEndQuery VKGL_ROUTE(EndQuery)
#undef glFenceSync
#define glFenceSync VKGL_ROUTE(FenceSync)
#undef glFinish
#define glFinish VKGL_ROUTE(Finish)
#undef glFlush
#define glFlush VKGL_ROUTE(Flush)
#undef glFramebufferParameteri
#define glFramebufferParameteri VKGL_ROUTE(FramebufferParameteri)
#undef glFramebufferTexture2D
#define glFramebufferTexture2D VKGL_ROUTE(FramebufferTexture2D)
#undef glGenBuffers
#define glGenBuffers VKGL_ROUTE(GenBuffers)
#undef glGenFramebuffers
#define glGenFramebuffers VKGL_ROUTE(GenFramebuffers)
#undef glGenQueries
#define glGenQueries VKGL_ROUTE(GenQueries)
#undef glGenSamplers
#define glGenSamplers VKGL_ROUTE(GenSamplers)
#undef glGenTextures
#define glGenTextures VKGL_ROUTE(GenTextures)
#undef glGenVertexArrays
#define glGenVertexArrays VKGL_ROUTE(GenVertexArrays)
#undef glGetError
#define glGetError VKGL_ROUTE(GetError)
#undef glGetIntegerv
#define glGetIntegerv VKGL_ROUTE(GetIntegerv)
#undef glGetProgramBinary
#define glGetProgramBinary VKGL_ROUTE(GetProgramBinary)
#undef glGetProgramInfoLog
#define glGetProgramInfoLog VKGL_ROUTE(GetProgramInfoLog)
#undef glGetProgramiv
#define glGetProgramiv VKGL_ROUTE(GetProgramiv)
#undef glGetQueryObjectiv
#define glGetQueryObjectiv VKGL_ROUTE(GetQueryObjectiv)
#undef glGetQueryObjectui64v
#define glGetQueryObjectui64v VKGL_ROUTE(GetQueryObjectui64v)
#undef glGetShaderInfoLog
#define glGetShaderInfoLog VKGL_ROUTE(GetShaderInfoLog)
#undef glGetShaderiv
#define glGetShaderiv VKGL_ROUTE(GetShaderiv)
#undef glGetString
#define glGetString VKGL_ROUTE(GetString)
#undef glGetStringi
#define glGetStringi VKGL_ROUTE(GetStringi)
#undef glGetTexImage
#define glGetTexImage VKGL_ROUTE(GetTexImage)
#undef glGetUniformLocation
#define glGetUniformLocation VKGL_ROUTE(GetUniformLocation)
#undef glLinkProgram
#define glLinkProgram VKGL_ROUTE(LinkProgram)
#undef glMapBufferRange
#define glMapBufferRange VKGL_ROUTE(MapBufferRange)
#undef glMemoryBarrier
#define glMemoryBarrier VKGL_ROUTE(MemoryBarrier)
#undef glPixelStorei
#define glPixelStorei VKGL_ROUTE(PixelStorei)
#undef glReadBuffer
#define glReadBuffer VKGL_ROUTE(ReadBuffer)
#undef glReadPixels
#define glReadPixels VKGL_ROUTE(ReadPixels)
#undef glSamplerParameteri
#define glSamplerParameteri VKGL_ROUTE(SamplerParameteri)
#undef glShaderSource
#define glShaderSource VKGL_ROUTE(ShaderSource)
#undef glTexParameteri
#define glTexParameteri VKGL_ROUTE(TexParameteri)
#undef glTexStorage2D
#define glTexStorage2D VKGL_ROUTE(TexStorage2D)
#undef glTexSubImage2D
#define glTexSubImage2D VKGL_ROUTE(TexSubImage2D)
#undef glUniform1f
#define glUniform1f VKGL_ROUTE(Uniform1f)
#undef glUniform1i
#define glUniform1i VKGL_ROUTE(Uniform1i)
#undef glUniform1ui
#define glUniform1ui VKGL_ROUTE(Uniform1ui)
#undef glUniform2f
#define glUniform2f VKGL_ROUTE(Uniform2f)
#undef glUniform2i
#define glUniform2i VKGL_ROUTE(Uniform2i)
#undef glUniform3i
#define glUniform3i VKGL_ROUTE(Uniform3i)
#undef glUniform4i
#define glUniform4i VKGL_ROUTE(Uniform4i)
#undef glUniform4f
#define glUniform4f VKGL_ROUTE(Uniform4f)
#undef glUniform3f
#define glUniform3f VKGL_ROUTE(Uniform3f)
#undef glUseProgram
#define glUseProgram VKGL_ROUTE(UseProgram)
#undef glViewport
#define glViewport VKGL_ROUTE(Viewport)
#endif
