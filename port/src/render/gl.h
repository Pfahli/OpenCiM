#pragma once
#include <cstddef>
#include <cstdint>

// Minimal OpenGL 3.3 core declarations, loaded at run time through the platform window (no system GL headers or
// third-party loaders). Only what the renderer uses.

#if defined(_WIN32)
#define REGS_GLAPI __stdcall
#else
#define REGS_GLAPI
#endif

namespace regs::gl {

using GLenum = unsigned int;
using GLuint = unsigned int;
using GLint = int;
using GLsizei = int;
using GLboolean = unsigned char;
using GLbitfield = unsigned int;
using GLfloat = float;
using GLchar = char;
using GLubyte = unsigned char;
using GLsizeiptr = std::ptrdiff_t;
using GLintptr = std::ptrdiff_t;

enum : GLenum {
  ZERO = 0, ONE = 1, TRIANGLES = 0x0004, LINES = 0x0001,
  SRC_ALPHA = 0x0302, ONE_MINUS_SRC_ALPHA = 0x0303,
  BLEND = 0x0BE2, POLYGON_OFFSET_FILL = 0x8037, SCISSOR_TEST = 0x0C11, DEPTH_TEST = 0x0B71, CULL_FACE = 0x0B44,
  TEXTURE_2D = 0x0DE1, TEXTURE0 = 0x84C0, TEXTURE_CUBE_MAP = 0x8513, TEXTURE_CUBE_MAP_POSITIVE_X = 0x8515,
  TEXTURE_WRAP_R = 0x8072, TEXTURE_CUBE_MAP_SEAMLESS = 0x884F,
  UNSIGNED_BYTE = 0x1401, FLOAT = 0x1406,
  RGBA = 0x1908, RGB = 0x1907, BGRA = 0x80E1, BGR = 0x80E0, RGBA8 = 0x8058, RGB8 = 0x8051,
  COMPRESSED_RGBA_S3TC_DXT1 = 0x83F1, COMPRESSED_RGBA_S3TC_DXT3 = 0x83F2, COMPRESSED_RGBA_S3TC_DXT5 = 0x83F3,
  TEXTURE_MAG_FILTER = 0x2800, TEXTURE_MIN_FILTER = 0x2801, TEXTURE_WRAP_S = 0x2802, TEXTURE_WRAP_T = 0x2803,
  TEXTURE_MAX_LEVEL = 0x813D, TEXTURE_MAX_ANISOTROPY = 0x84FE,
  LINEAR = 0x2601, NEAREST = 0x2600, LINEAR_MIPMAP_LINEAR = 0x2703, REPEAT = 0x2901, CLAMP_TO_EDGE = 0x812F,
  UNPACK_ALIGNMENT = 0x0CF5, PACK_ALIGNMENT = 0x0D05,
  ARRAY_BUFFER = 0x8892, ELEMENT_ARRAY_BUFFER = 0x8893, STREAM_DRAW = 0x88E0, STATIC_DRAW = 0x88E4,
  UNSIGNED_INT = 0x1405, SHORT = 0x1402, LEQUAL = 0x0203, LESS = 0x0201, BACK = 0x0405, FRONT = 0x0404, CCW = 0x0901,
  FRAMEBUFFER = 0x8D40, RENDERBUFFER = 0x8D41, COLOR_ATTACHMENT0 = 0x8CE0, DEPTH_ATTACHMENT = 0x8D00,
  DEPTH_COMPONENT24 = 0x81A6, FRAMEBUFFER_COMPLETE = 0x8CD5, VIEWPORT = 0x0BA2, SCISSOR_BOX = 0x0C10,
  FRAGMENT_SHADER = 0x8B30, VERTEX_SHADER = 0x8B31, COMPILE_STATUS = 0x8B81, LINK_STATUS = 0x8B82,
  COLOR_BUFFER_BIT = 0x4000, DEPTH_BUFFER_BIT = 0x0100,
  VENDOR = 0x1F00, RENDERER = 0x1F01, VERSION = 0x1F02,
  CLAMP_TO_BORDER = 0x812D, TEXTURE_BORDER_COLOR = 0x1004,
  RGBA16F = 0x881A, HALF_FLOAT = 0x140B, DEPTH_COMPONENT = 0x1902, NONE = 0, RED = 0x1903,
  TEXTURE_COMPARE_MODE = 0x884C, TEXTURE_COMPARE_FUNC = 0x884D, COMPARE_REF_TO_TEXTURE = 0x884E,
  READ_FRAMEBUFFER = 0x8CA8, DRAW_FRAMEBUFFER = 0x8CA9, MAX_SAMPLES = 0x8D57, NEAREST_MIPMAP_NEAREST = 0x2700,
};

#define REGS_GL_FUNCS(X)                                                                                    \
  X(void, Clear, GLbitfield)                                                                                \
  X(void, ClearColor, GLfloat, GLfloat, GLfloat, GLfloat)                                                   \
  X(void, Viewport, GLint, GLint, GLsizei, GLsizei)                                                         \
  X(void, Scissor, GLint, GLint, GLsizei, GLsizei)                                                          \
  X(void, Enable, GLenum)                                                                                   \
  X(void, Disable, GLenum)                                                                                  \
  X(void, BlendFunc, GLenum, GLenum)                                                                        \
  X(void, PolygonOffset, GLfloat, GLfloat)                                                                  \
  X(void, DrawBuffer, GLenum)                                                                               \
  X(void, ReadBuffer, GLenum)                                                                               \
  X(void, ColorMask, GLboolean, GLboolean, GLboolean, GLboolean)                                            \
  X(void, GetTexImage, GLenum, GLint, GLenum, GLenum, void*)                                                \
  X(void, BlitFramebuffer, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum)      \
  X(void, RenderbufferStorageMultisample, GLenum, GLsizei, GLenum, GLsizei, GLsizei)                        \
  X(const GLubyte*, GetString, GLenum)                                                                      \
  X(GLenum, GetError)                                                                                       \
  X(void, PixelStorei, GLenum, GLint)                                                                       \
  X(void, ReadPixels, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*)                                \
  X(void, GenTextures, GLsizei, GLuint*)                                                                    \
  X(void, DeleteTextures, GLsizei, const GLuint*)                                                           \
  X(void, BindTexture, GLenum, GLuint)                                                                      \
  X(void, TexParameteri, GLenum, GLenum, GLint)                                                             \
  X(void, TexParameterf, GLenum, GLenum, GLfloat)                                                           \
  X(void, TexParameterfv, GLenum, GLenum, const GLfloat*)                                                   \
  X(void, TexImage2D, GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)           \
  X(void, TexSubImage2D, GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*)        \
  X(void, CompressedTexImage2D, GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*)       \
  X(void, ActiveTexture, GLenum)                                                                            \
  X(void, DrawArrays, GLenum, GLint, GLsizei)                                                               \
  X(void, GenBuffers, GLsizei, GLuint*)                                                                     \
  X(void, DeleteBuffers, GLsizei, const GLuint*)                                                            \
  X(void, BindBuffer, GLenum, GLuint)                                                                       \
  X(void, BufferData, GLenum, GLsizeiptr, const void*, GLenum)                                              \
  X(void, BufferSubData, GLenum, GLintptr, GLsizeiptr, const void*)                                         \
  X(void, GenVertexArrays, GLsizei, GLuint*)                                                                \
  X(void, BindVertexArray, GLuint)                                                                          \
  X(void, EnableVertexAttribArray, GLuint)                                                                  \
  X(void, VertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)                      \
  X(GLuint, CreateShader, GLenum)                                                                           \
  X(void, ShaderSource, GLuint, GLsizei, const GLchar* const*, const GLint*)                                \
  X(void, CompileShader, GLuint)                                                                            \
  X(void, GetShaderiv, GLuint, GLenum, GLint*)                                                              \
  X(void, GetShaderInfoLog, GLuint, GLsizei, GLsizei*, GLchar*)                                             \
  X(void, DeleteShader, GLuint)                                                                             \
  X(GLuint, CreateProgram)                                                                                  \
  X(void, AttachShader, GLuint, GLuint)                                                                     \
  X(void, BindAttribLocation, GLuint, GLuint, const GLchar*)                                                \
  X(void, LinkProgram, GLuint)                                                                              \
  X(void, GetProgramiv, GLuint, GLenum, GLint*)                                                             \
  X(void, GetProgramInfoLog, GLuint, GLsizei, GLsizei*, GLchar*)                                            \
  X(void, UseProgram, GLuint)                                                                               \
  X(GLint, GetUniformLocation, GLuint, const GLchar*)                                                       \
  X(void, Uniform1i, GLint, GLint)                                                                          \
  X(void, Uniform1f, GLint, GLfloat)                                                                        \
  X(void, Uniform3f, GLint, GLfloat, GLfloat, GLfloat)                                                      \
  X(void, Uniform4f, GLint, GLfloat, GLfloat, GLfloat, GLfloat)                                             \
  X(void, UniformMatrix4fv, GLint, GLsizei, GLboolean, const GLfloat*)                                      \
  X(void, UniformMatrix3fv, GLint, GLsizei, GLboolean, const GLfloat*)                                      \
  X(void, Uniform3fv, GLint, GLsizei, const GLfloat*)                                                       \
  X(void, Uniform4fv, GLint, GLsizei, const GLfloat*)                                                       \
  X(void, DrawElements, GLenum, GLsizei, GLenum, const void*)                                               \
  X(void, DepthMask, GLboolean)                                                                             \
  X(void, DepthFunc, GLenum)                                                                                \
  X(void, CullFace, GLenum)                                                                                 \
  X(void, ClearDepth, double)                                                                               \
  X(void, GetIntegerv, GLenum, GLint*)                                                                      \
  X(void, GenerateMipmap, GLenum)                                                                           \
  X(void, GenFramebuffers, GLsizei, GLuint*)                                                                \
  X(void, DeleteFramebuffers, GLsizei, const GLuint*)                                                       \
  X(void, BindFramebuffer, GLenum, GLuint)                                                                  \
  X(void, FramebufferTexture2D, GLenum, GLenum, GLenum, GLuint, GLint)                                      \
  X(void, GenRenderbuffers, GLsizei, GLuint*)                                                               \
  X(void, DeleteRenderbuffers, GLsizei, const GLuint*)                                                      \
  X(void, BindRenderbuffer, GLenum, GLuint)                                                                 \
  X(void, RenderbufferStorage, GLenum, GLenum, GLsizei, GLsizei)                                            \
  X(void, FramebufferRenderbuffer, GLenum, GLenum, GLenum, GLuint)                                          \
  X(GLenum, CheckFramebufferStatus, GLenum)                                                                 \
  X(void, DeleteVertexArrays, GLsizei, const GLuint*)

#define REGS_GL_DECLARE(ret, name, ...) using PFN_##name = ret(REGS_GLAPI*)(__VA_ARGS__); extern PFN_##name name;
REGS_GL_FUNCS(REGS_GL_DECLARE)
#undef REGS_GL_DECLARE

// Resolves every entry point through 'proc'; returns the name of the first missing one, or null.
const char* load(void* (*proc)(const char* name, void* user), void* user);

}  // namespace regs::gl
