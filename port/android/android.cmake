# The Android build (docs/android.md), included by CMakeLists.txt with the NDK's toolchain: the game as libmain.so
# (SDL3's SDL_main, started by GameActivity) with the Steam client and the JNI functions of the setup screen
# (src/platform/android_jni.cpp). android/build.sh builds it for each ABI and packs the APK.

set(Z ${CMAKE_SOURCE_DIR}/third_party/zstd)
set(REGS_STEAM_C
  third_party/lzma/LzmaDec.c
  ${Z}/common/debug.c ${Z}/common/entropy_common.c ${Z}/common/error_private.c ${Z}/common/fse_decompress.c
  ${Z}/common/xxhash.c ${Z}/common/zstd_common.c
  ${Z}/decompress/huf_decompress.c ${Z}/decompress/zstd_ddict.c ${Z}/decompress/zstd_decompress.c
  ${Z}/decompress/zstd_decompress_block.c)
set_source_files_properties(${REGS_STEAM_C} third_party/qrcodegen/qrcodegen.cpp PROPERTIES COMPILE_OPTIONS "-w")
add_library(regs_steam STATIC src/core/steam_client.cpp src/core/steam_crypto.cpp src/core/net_curl.cpp
  third_party/qrcodegen/qrcodegen.cpp ${REGS_STEAM_C})
target_include_directories(regs_steam PRIVATE third_party/lzma ${Z} PUBLIC third_party/qrcodegen)
target_compile_definitions(regs_steam PRIVATE ZSTD_DISABLE_ASM)
target_link_libraries(regs_steam PUBLIC regs_engine CURL::libcurl)

add_library(main SHARED src/app/main.cpp src/platform/android_jni.cpp ${REGS_CRASH_SRC})
target_link_libraries(main PRIVATE regs_engine regs_steam android log)
# 16 KB pages (devices from Android 15 on may use them); unused code out
target_link_options(main PRIVATE -Wl,-z,max-page-size=16384 -Wl,--gc-sections)
