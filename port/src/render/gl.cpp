#include "render/gl.h"

namespace regs::gl {

#define REGS_GL_DEFINE(ret, name, ...) PFN_##name name = nullptr;
REGS_GL_FUNCS(REGS_GL_DEFINE)
#undef REGS_GL_DEFINE

const char* load(void* (*proc)(const char* name, void* user), void* user) {
#define REGS_GL_LOAD(ret, name, ...)                                     \
  name = reinterpret_cast<PFN_##name>(proc("gl" #name, user));          \
  if (!name) return "gl" #name;
  REGS_GL_FUNCS(REGS_GL_LOAD)
#undef REGS_GL_LOAD
  return nullptr;
}

}  // namespace regs::gl
