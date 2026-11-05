cmake_minimum_required(VERSION 3.20)
project(regs VERSION 0.1.0 LANGUAGES CXX)

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
  src/engine/sky.cpp
  src/engine/interaction.cpp
  src/engine/tools.cpp
  src/engine/tool_objects.cpp
  src/engine/tool_tracks.cpp
  src/engine/grid_save.cpp
  src/engine/grid_edit.cpp
  src/engine/timeline.cpp
  src/engine/sim.cpp
  src/engine/sim_graph.cpp
  src/engine/sim_air.cpp
  src/engine/sim_citizens.cpp
  src/engine/sim_save.cpp
  src/engine/sim_lines.cpp
  src/engine/tool_addstop.cpp
  src/engine/ui_lines.cpp
  src/engine/ui_graphs.cpp
  src/engine/sim_paths.cpp
  src/engine/sound.cpp
  src/engine/sim_sounds.cpp
  src/engine/sim_doors.cpp
  src/engine/sim_jams.cpp
  src/engine/particles.cpp
  src/platform/window_win32.cpp
  src/audio/audio.cpp
  src/audio/miniaudio_impl.cpp
)
target_include_directories(regs_engine PUBLIC src)
# the version and the build's git state (build_info.h: REGS_VERSION_STRING, REGS_BUILD_NUMBER = commit count,
# REGS_BUILD_COMMIT, REGS_BUILD_DATE), refreshed at every build; the main menu shows them
add_custom_target(regs_build_info
  COMMAND ${CMAKE_COMMAND} -DSRC=${CMAKE_SOURCE_DIR} -DOUT=${CMAKE_BINARY_DIR}/generated/build_info.h
          -DVERSION=${PROJECT_VERSION} -P ${CMAKE_SOURCE_DIR}/cmake/build_info.cmake
  BYPRODUCTS ${CMAKE_BINARY_DIR}/generated/build_info.h)
add_dependencies(regs_engine regs_build_info)
target_include_directories(regs_engine PUBLIC ${CMAKE_BINARY_DIR}/generated)
# third-party: miniaudio (public domain / MIT-0), the audio output and the WAV/MP3 decoders; its implementation file is
# compiled without the port's warning level
target_include_directories(regs_engine PRIVATE third_party/miniaudio)
if(MSVC)
  set_source_files_properties(src/audio/miniaudio_impl.cpp PROPERTIES COMPILE_OPTIONS "/W0")
else()
  set_source_files_properties(src/audio/miniaudio_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")
endif()

add_executable(regs-tool src/tools/regs_tool.cpp src/tools/crash_win32.cpp)
target_link_libraries(regs-tool PRIVATE regs_engine dbghelp)

# the game
add_executable(regs src/app/main.cpp src/tools/crash_win32.cpp)
target_link_libraries(regs PRIVATE regs_engine dbghelp)
# the port's own game files (menu pages, strings, script extensions), mounted over the install from data/ next to the
# executable; copied on every build (also when only data/ changed and nothing relinks)
add_custom_target(regs_data ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/data $<TARGET_FILE_DIR:regs>/data)
add_dependencies(regs_data regs)
if(WIN32)
  target_link_libraries(regs_engine PUBLIC user32 gdi32 opengl32)
endif()

