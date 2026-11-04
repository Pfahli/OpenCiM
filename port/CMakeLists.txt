cmake_minimum_required(VERSION 3.20)
project(regs LANGUAGES CXX)

# ReGS: a from-source reimplementation of the GSystem engine that runs Cities in Motion from an installed copy.
# No game data is part of this tree; everything is read from the user's install at run time.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
  add_compile_options(/utf-8 /W4 /permissive- /EHsc /Zc:__cplusplus /wd4100 /wd4505)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
else()
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

add_library(regs_engine STATIC
  src/core/log.cpp
  src/core/text.cpp
  src/core/inflate.cpp
  src/core/deflate.cpp
  src/vfs/archive.cpp
  src/vfs/vfs.cpp
  src/script/lexer.cpp
  src/script/parser.cpp
  src/script/value.cpp
  src/script/object.cpp
  src/script/compiler.cpp
  src/script/vm.cpp
  src/script/vm_ops.cpp
  src/engine/api.cpp
  src/engine/api_generated.cpp
  src/engine/natives_core.cpp
  src/engine/natives_objects.cpp
  src/engine/natives_ui.cpp
  src/engine/engine.cpp
  src/engine/platform_win32.cpp
  src/engine/display.cpp
  src/engine/font.cpp
  src/engine/input.cpp
  src/render/gl.cpp
  src/render/image.cpp
  src/render/renderer2d.cpp
  src/render/renderer3d.cpp
  src/render/mesh.cpp
  src/render/skeleton.cpp
  src/render/glsl.cpp
  src/engine/scene3d.cpp
  src/engine/shaders.cpp
  src/engine/grid.cpp
  src/engine/terrain.cpp
  src/engine/grid_items.cpp
  src/engine/effects.cpp
  src/engine/settings.cpp
  src/engine/minimap.cpp
  src/engine/skinning.cpp
  src/engine/interaction.cpp
  src/engine/tools.cpp
  src/engine/grid_save.cpp
  src/engine/grid_edit.cpp
  src/engine/timeline.cpp
  src/engine/sim.cpp
  src/engine/sim_graph.cpp
  src/engine/sim_air.cpp
  src/engine/sim_citizens.cpp
  src/engine/sim_save.cpp
  src/platform/window_win32.cpp
)
target_include_directories(regs_engine PUBLIC src)

add_executable(regs-tool src/tools/regs_tool.cpp src/tools/crash_win32.cpp)
target_link_libraries(regs-tool PRIVATE regs_engine dbghelp)

# the game
add_executable(regs src/app/main.cpp src/tools/crash_win32.cpp)
target_link_libraries(regs PRIVATE regs_engine dbghelp)
# the port's own game files (menu pages, strings), mounted over the install from data/ next to the executable
add_custom_command(TARGET regs POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/data $<TARGET_FILE_DIR:regs>/data)
if(WIN32)
  target_link_libraries(regs_engine PUBLIC user32 gdi32 opengl32)
endif()

