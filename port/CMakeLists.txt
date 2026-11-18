cmake_minimum_required(VERSION 3.20)
project(regs VERSION 0.2.0 LANGUAGES CXX)

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
  src/engine/gui_relayout.cpp
  src/engine/font.cpp
  src/engine/input.cpp
  src/engine/camera_input.cpp
  src/engine/gamepad.cpp
  src/render/gl.cpp
  src/render/gpu_gl.cpp
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
  src/engine/temporal.cpp
  src/engine/hdr.cpp
  src/engine/settings.cpp
  src/engine/minimap.cpp
  src/engine/skinning.cpp
  src/engine/sky.cpp
  src/engine/night_lights.cpp
  src/engine/texture_upscale.cpp
  src/engine/traffic_lights.cpp
  src/engine/interaction.cpp
  src/engine/tools.cpp
  src/engine/tool_objects.cpp
  src/engine/tool_templates.cpp
  src/engine/osm.cpp
  src/engine/tool_curves.cpp
  src/engine/curve_roads.cpp
  src/engine/proc_buildings.cpp
  src/engine/tool_terrain.cpp
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
  src/engine/mods.cpp
  src/engine/paint_edges.cpp
  src/engine/surface_map.cpp
  src/engine/traffic_maps.cpp
  src/engine/snowplows.cpp
  src/engine/user_folders.cpp
  src/engine/debug_economy.cpp
  src/platform/window_win32.cpp
  src/platform/touchpad_win32.cpp
  src/platform/gamepad_win32.cpp
  src/platform/steam_hid_win32.cpp
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

# Vulkan render backend (docs/renderer/vulkan.md §6): Khronos Vulkan-Headers and glslang (GLSL -> SPIR-V at run time)
# from third_party; without the headers the port builds with OpenGL only and the Vulkan setting falls back to OpenGL.
# Without glslang the Vulkan device builds but compiles no shader (bring-up only: clears and presents). vulkan-1.dll
# is loaded at run time, never linked. REGS_VULKAN_HEADERS may point at another Vulkan-Headers include directory.
set(REGS_VULKAN_HEADERS ${CMAKE_SOURCE_DIR}/third_party/vulkan-headers/include CACHE PATH "Vulkan-Headers include directory")
set(REGS_VULKAN_MODE AUTO CACHE STRING "Vulkan render backend: AUTO (when the headers are found), ON or OFF")
set_property(CACHE REGS_VULKAN_MODE PROPERTY STRINGS AUTO ON OFF)
if(REGS_VULKAN_MODE STREQUAL "AUTO")
  if(EXISTS ${REGS_VULKAN_HEADERS}/vulkan/vulkan.h)
    set(REGS_VULKAN ON)
  else()
    set(REGS_VULKAN OFF)
  endif()
else()
  set(REGS_VULKAN ${REGS_VULKAN_MODE})
endif()
if(REGS_VULKAN AND NOT EXISTS ${REGS_VULKAN_HEADERS}/vulkan/vulkan.h)
  message(WARNING "REGS_VULKAN: no vulkan/vulkan.h under ${REGS_VULKAN_HEADERS}, building without Vulkan")
  set(REGS_VULKAN OFF)
endif()
if(REGS_VULKAN)
  target_sources(regs_engine PRIVATE src/render/gpu_vk.cpp)
  target_include_directories(regs_engine PRIVATE ${REGS_VULKAN_HEADERS})
  target_compile_definitions(regs_engine PRIVATE REGS_VULKAN=1 VK_NO_PROTOTYPES VK_USE_PLATFORM_WIN32_KHR)
endif()
if(REGS_VULKAN AND EXISTS ${CMAKE_SOURCE_DIR}/third_party/glslang/CMakeLists.txt)
  set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
  set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
  set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
  set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
  set(ENABLE_CTEST OFF CACHE BOOL "" FORCE)
  set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
  set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
  set(BUILD_EXTERNAL OFF CACHE BOOL "" FORCE)
  set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  add_subdirectory(third_party/glslang EXCLUDE_FROM_ALL)
  target_sources(regs_engine PRIVATE src/render/vk_shader.cpp)
  target_link_libraries(regs_engine PRIVATE glslang::glslang glslang::glslang-default-resource-limits)
  # AMD FSR 3.1 upscaling (third_party/fidelityfx-fsr, FidelityFX SDK v1.1.4, MIT; docs/renderer/vulkan.md §9.2). The
  # shaders are compiled at build time by the port's own ffx-shadergen (glslang) instead of AMD's prebuilt tools; only
  # the permutations the port uses are built (the fixed options below match render/fsr_vk.cpp's context flags).
  set(FFX_DIR ${CMAKE_SOURCE_DIR}/third_party/fidelityfx-fsr)
  if(EXISTS ${FFX_DIR}/LICENSE.txt)
    add_executable(ffx-shadergen src/tools/ffx_shadergen.cpp)
    target_link_libraries(ffx-shadergen PRIVATE glslang::glslang glslang::glslang-default-resource-limits)
    set(FFX_GEN ${CMAKE_BINARY_DIR}/ffx_shaders)
    set(FFX_GPU ${FFX_DIR}/include/FidelityFX/gpu)
    set(FSR3U_ARGS
      -I ${FFX_GPU} -I ${FFX_GPU}/fsr3upscaler
      -D FFX_GPU=1 -D FFX_GLSL=1
      -D FFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0 -D FFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
      -D FFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1 -D FFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
      -D FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2
      --option FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE --option FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT
      --option FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS --option FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS
      --option FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH --option FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING
      --fixed FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=0 --fixed FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS=1
      --fixed FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS=0 --fixed FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH=0)
    set(FFX_GENERATED)
    foreach(pass accumulate autogen_reactive debug_view luma_instability luma_pyramid prepare_inputs prepare_reactivity rcas
                 shading_change shading_change_pyramid)
      set(name ffx_fsr3upscaler_${pass}_pass)
      set(src ${FFX_DIR}/src/backends/vk/shaders/fsr3upscaler/${name}.glsl)
      add_custom_command(
        OUTPUT ${FFX_GEN}/${name}_permutations.h ${FFX_GEN}/${name}_wave64_permutations.h
               ${FFX_GEN}/${name}_16bit_permutations.h ${FFX_GEN}/${name}_wave64_16bit_permutations.h
        COMMAND ffx-shadergen --source ${src} --name ${name} --out ${FFX_GEN} ${FSR3U_ARGS}
        DEPENDS ffx-shadergen ${src}
        COMMENT "ffx-shadergen ${name}")
      list(APPEND FFX_GENERATED ${FFX_GEN}/${name}_permutations.h)
    endforeach()
    # FSR 3 frame generation (docs/renderer/vulkan.md §9.4): frame interpolation and optical flow; the fixed options match
    # render/fsr_vk.cpp (motion vectors without jitter, depth 0 near .. 1 far); low-resolution vectors and the optical
    # flow's HDR (PQ) input vary
    set(FI_ARGS
      -I ${FFX_GPU} -I ${FFX_GPU}/frameinterpolation -D FFX_GPU=1 -D FFX_GLSL=1
      -D FFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0 -D FFX_FRAMEINTERPOLATION_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0
      -D FFX_FRAMEINTERPOLATION_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1 -D FFX_FRAMEINTERPOLATION_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0
      -D FFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2
      --option FFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS --option FFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS
      --option FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH
      --fixed FFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS=0 --fixed FFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH=0)
    set(OF_ARGS -I ${FFX_GPU} -I ${FFX_GPU}/opticalflow -D FFX_GPU=1 -D FFX_GLSL=1 --option FFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT)
    foreach(pass reconstruct_and_dilate disocclusion_mask reconstruct_previous_depth setup game_motion_vector_field
                 optical_flow_vector_field compute_game_vector_field_inpainting_pyramid compute_inpainting_pyramid inpainting
                 debug_view)
      list(APPEND FFX_FI_PASSES ffx_frameinterpolation_${pass}_pass)
    endforeach()
    list(APPEND FFX_FI_PASSES ffx_frameinterpolation_pass)
    foreach(name ${FFX_FI_PASSES})
      set(src ${FFX_DIR}/src/backends/vk/shaders/frameinterpolation/${name}.glsl)
      add_custom_command(
        OUTPUT ${FFX_GEN}/${name}_permutations.h ${FFX_GEN}/${name}_wave64_permutations.h
               ${FFX_GEN}/${name}_16bit_permutations.h ${FFX_GEN}/${name}_wave64_16bit_permutations.h
        COMMAND ffx-shadergen --source ${src} --name ${name} --out ${FFX_GEN} ${FI_ARGS}
        DEPENDS ffx-shadergen ${src}
        COMMENT "ffx-shadergen ${name}")
      list(APPEND FFX_GENERATED ${FFX_GEN}/${name}_permutations.h)
    endforeach()
    foreach(pass compute_luminance_pyramid compute_optical_flow_advanced compute_scd_divergence filter_optical_flow
                 generate_scd_histogram prepare_luma scale_optical_flow_advanced)
      set(name ffx_opticalflow_${pass}_pass)
      if(pass MATCHES "advanced$" OR pass STREQUAL "filter_optical_flow")
        set(name ${name}_v5)
      endif()
      set(src ${FFX_DIR}/src/backends/vk/shaders/opticalflow/${name}.glsl)
      add_custom_command(
        OUTPUT ${FFX_GEN}/${name}_permutations.h ${FFX_GEN}/${name}_wave64_permutations.h
               ${FFX_GEN}/${name}_16bit_permutations.h ${FFX_GEN}/${name}_wave64_16bit_permutations.h
        COMMAND ffx-shadergen --source ${src} --name ${name} --out ${FFX_GEN} ${OF_ARGS}
        DEPENDS ffx-shadergen ${src}
        COMMENT "ffx-shadergen ${name}")
      list(APPEND FFX_GENERATED ${FFX_GEN}/${name}_permutations.h)
    endforeach()
    add_library(regs_ffx STATIC
      ${FFX_DIR}/src/shared/ffx_assert.cpp
      ${FFX_DIR}/src/shared/ffx_message.cpp
      ${FFX_DIR}/src/shared/ffx_object_management.cpp
      ${FFX_DIR}/src/shared/ffx_breadcrumbs_list.cpp
      ${FFX_DIR}/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp
      ${FFX_DIR}/src/backends/vk/ffx_vk.cpp
      ${FFX_DIR}/src/backends/shared/ffx_shader_blobs.cpp
      ${FFX_DIR}/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp
      ${FFX_DIR}/src/components/frameinterpolation/ffx_frameinterpolation.cpp
      ${FFX_DIR}/src/components/opticalflow/ffx_opticalflow.cpp
      ${FFX_DIR}/src/backends/shared/blob_accessors/ffx_frameinterpolation_shaderblobs.cpp
      ${FFX_DIR}/src/backends/shared/blob_accessors/ffx_opticalflow_shaderblobs.cpp
      src/render/fsr_vk.cpp
      ${FFX_GENERATED})
    target_include_directories(regs_ffx PRIVATE ${FFX_DIR}/include ${FFX_DIR}/src ${FFX_DIR}/src/components ${FFX_DIR}/src/shared
                               ${FFX_DIR}/src/backends/shared ${FFX_GEN} ${REGS_VULKAN_HEADERS} ${CMAKE_SOURCE_DIR}/src)
    target_compile_definitions(regs_ffx PRIVATE FFX_FSR3UPSCALER FFX_FI FFX_OF VK_USE_PLATFORM_WIN32_KHR _UNICODE UNICODE)
    if(MSVC)
      target_compile_options(regs_ffx PRIVATE /W0 /FIbit)
    endif()
    target_link_libraries(regs_engine PRIVATE regs_ffx)
    target_compile_definitions(regs_engine PRIVATE REGS_FFX=1)
  endif()
elseif(REGS_VULKAN)
  target_sources(regs_engine PRIVATE src/render/vk_shader_stub.cpp)
else()
  target_sources(regs_engine PRIVATE src/render/gpu_vk_stub.cpp)
endif()

add_executable(regs-tool src/tools/regs_tool.cpp src/tools/mod_assets.cpp src/tools/pad_glyphs.cpp src/tools/texture_tools.cpp src/tools/crash_win32.cpp)
target_include_directories(regs-tool PRIVATE third_party/miniaudio)
target_link_libraries(regs-tool PRIVATE regs_engine dbghelp)

# the game
add_executable(regs src/app/main.cpp src/tools/crash_win32.cpp src/tools/profiler_win32.cpp)
target_link_libraries(regs PRIVATE regs_engine dbghelp winmm)
# the port's own game files (menu pages, strings, script extensions), mounted over the install from data/ next to the
# executable; copied on every build (also when only data/ changed and nothing relinks)
# Built-in mods (data/mods, disabled until listed in the user's mods.json) come along; the weather mod's seamless loops
# are cut here from its CC0 recordings by regs-tool cut (data/mods/weather/sounds/LICENSES.txt), its soft particle
# sprite drawn by regs-tool softdot, its falling-leaf sprite by regs-tool leaf; the controller legend's button glyphs
# by regs-tool padglyphs (data/regs/ui/pad, docs/controller.md)
set(WEATHER_SOUNDS $<TARGET_FILE_DIR:regs>/data/mods/weather/sounds)
add_custom_target(regs_data ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/data $<TARGET_FILE_DIR:regs>/data
  COMMAND $<TARGET_FILE:regs-tool> cut ${WEATHER_SOUNDS}/light-rain.mp3 ${WEATHER_SOUNDS}/rain-light.wav 2 20 2 loop
  COMMAND $<TARGET_FILE:regs-tool> cut ${WEATHER_SOUNDS}/slowly-raining-loop.mp3 ${WEATHER_SOUNDS}/rain-normal.wav 5 30 3 loop
  COMMAND $<TARGET_FILE:regs-tool> cut ${WEATHER_SOUNDS}/heavy-rain-roof.mp3 ${WEATHER_SOUNDS}/rain-heavy.wav 10 30 3 loop
  COMMAND $<TARGET_FILE:regs-tool> cut ${WEATHER_SOUNDS}/cars-rainy-street.mp3 ${WEATHER_SOUNDS}/wet-traffic.wav 2 30 3 loop
  COMMAND $<TARGET_FILE:regs-tool> cut ${WEATHER_SOUNDS}/thunder-clap.mp3 ${WEATHER_SOUNDS}/thunder-mid.wav 0 14 4
  COMMAND $<TARGET_FILE:regs-tool> softdot $<TARGET_FILE_DIR:regs>/data/mods/weather/textures/softdot.dds 32
  COMMAND $<TARGET_FILE:regs-tool> leaf $<TARGET_FILE_DIR:regs>/data/mods/weather/textures/leaf.dds 32
  COMMAND $<TARGET_FILE:regs-tool> padglyphs $<TARGET_FILE_DIR:regs>/data/regs/ui/pad)
add_dependencies(regs_data regs regs-tool)
if(WIN32)
  target_link_libraries(regs_engine PUBLIC user32 gdi32 opengl32 shell32 winhttp windowscodecs ole32 hid setupapi)
endif()

