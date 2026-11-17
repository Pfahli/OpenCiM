# Third-party code

Everything here is redistributable source built with the port. Each folder keeps its upstream licence files.

| Folder | What | Version / source | Licence | Used for |
|---|---|---|---|---|
| `miniaudio/` | miniaudio (single header) | github.com/mackron/miniaudio | public domain or MIT-0 | audio output, WAV/MP3 decoding |
| `vulkan-headers/` | Khronos Vulkan-Headers: `include/vulkan/{vk_platform,vulkan,vulkan_core,vulkan_win32}.h` and `include/vk_video/` only (no `vulkan.hpp`, no registry) | tag `vulkan-sdk-1.4.357.0`, github.com/KhronosGroup/Vulkan-Headers | Apache-2.0 OR MIT (`LICENSE.md`, `LICENSES/`) | the Vulkan render backend (`src/render/gpu_vk.cpp`); `vulkan-1.dll` is loaded at run time, nothing is linked |
| `glslang/` | Khronos glslang: `glslang/`, `SPIRV/`, `StandAlone/` (one header the C interface includes; the tool itself is not built), CMake files, `CHANGES.md` (version parsing); tests, Android/GN files and `External/` left out | tag `vulkan-sdk-1.4.357.0`, github.com/KhronosGroup/glslang | mostly BSD-3-Clause; parts BSD-2-Clause, MIT, Apache-2.0 and the NVIDIA "AML-glslang" licence; the generated parser `glslang/MachineIndependent/glslang_tab.cpp(.h)` is GPL-3.0-or-later WITH Bison-exception-2.2, whose exception allows it in a program under other terms (`LICENSE.txt`, `LICENSES/`, `REUSE.toml`) | GLSL to SPIR-V at run time for the Vulkan backend (`src/render/vk_shader.cpp`) |

Updating Vulkan-Headers or glslang: take both at the same `vulkan-sdk-*` tag, copy the same subset, and update this
table. Release builds must ship these licence texts with the program.
