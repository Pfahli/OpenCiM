#pragma once
#include <string>

namespace regs::render {

// Rewrites a shipped GLSL 1.10 shader (written against fixed-function built-ins) as GLSL 3.30 core:
// attribute/varying → in/out, texture2D/textureCube → texture, shadow2D → a vec4 wrapper, gl_FragColor → an output,
// and every built-in the shipped shaders read (gl_Vertex, gl_Normal, gl_Color, gl_MultiTexCoord0-7, gl_FrontColor,
// the matrices, gl_FrontMaterial, gl_LightSource[], gl_FrontLightProduct[], gl_LightModel, gl_Fog) → regs_* inputs
// and uniforms with the same members. Vertex inputs carry no locations; the program builder binds them.
// Returns "" when the source uses another built-in.
std::string translateGlsl110(const std::string& src, bool fragment, std::string* unsupported);

}  // namespace regs::render
