#pragma once
// Upscaled textures (port feature, docs/renderer/texture-upscaling.md). The port never ships game data, so the
// upscaled copies are made on the player's machine from their own install and kept in a cache below the OpenCiM
// folder (texture-cache\x2\<game path>.dds). Each cached file names the exact source bytes it was made from, so a
// changed install file simply falls back to the original until it is made again.
//
// A cached texture is the source with one more level on top: level 0 is the 2Ã— upscale, levels 1.. are the source's
// own levels byte for byte. Wherever the original was minified (most of the city view) the GPU samples the very same
// texels as before; only magnified surfaces (close-ups, UI scale 1.5/2) use the new level.
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "render/image.h"

namespace regs {
class Vfs;
}

namespace regs::engine::texup {

// What a texture holds, which decides how it is scaled (texture-upscaling.md Â§3).
enum class Kind : uint8_t { Skip, Color, AlphaTest, Normal, Mask, Ui };
const char* kindName(Kind k);

// Bumped when the filter changes: older cache files are then ignored and made again.
inline constexpr uint32_t kCacheVersion = 3;
// Bumped when classify() selects more textures: complete caches then count as incomplete and only the new ones are
// made (the existing files stay valid). 2: the 2048-wide loading-screen and main-menu art.
inline constexpr uint32_t kSelectionRules = 2;

// Identity of a source file (64-bit FNV-1a of its bytes, mixed with the size).
uint64_t sourceStamp(const std::vector<uint8_t>& file);

// Material roles of every texture the install's .material and .object files name (diffuse, alpha-tested diffuse,
// normal, specular / illumination / ambient-occlusion masks).
using Roles = std::map<std::string, Kind>;
Roles scanRoles(const Vfs& vfs, const std::function<bool(const std::string&, std::vector<uint8_t>&)>& read,
                const std::atomic<bool>* stop = nullptr);

// The kind of one texture: its material role when it has one, else by folder and name (UI art, terrain, _n/_s/_ao
// suffixes); Skip for cube maps, fonts, effects, lookup textures, tiny and very large ones.
Kind classify(const std::string& path, const Roles& roles, const render::Image& img, bool includeUi = true);

// 2Ã— level 0 as RGBA8 (top row first), from the built-in filter (Lanczos-3 with anti-ringing, contrast-adaptive
// sharpening for colour, renormalised normals, crisp 1-bit alpha edges). wrap: the texture repeats (3D surfaces).
bool upscaleLevel0(const render::Image& src, Kind kind, bool wrap, std::vector<uint8_t>& rgba2x, int& w2, int& h2);

// The cached texture: rgba2x (2Ã— of the source's level 0, RGBA8) encoded in the source's format on top of the
// source's own levels. A source without mipmaps (UI) gets its original as level 1, so at UI scale 1 it looks as before.
bool buildUpscaled(const render::Image& src, const std::vector<uint8_t>& rgba2x, render::Image& out);

// DDS file of an image (DXT1/3/5, BGRA8, BGR8; no cube maps) with the cache stamp in the header's reserved words.
std::vector<uint8_t> writeDds(const render::Image& img, uint64_t stamp, Kind kind, bool external);
// The stamp of a cache file written by writeDds (false for other files or another kCacheVersion).
bool readStamp(const std::vector<uint8_t>& dds, uint64_t& stamp, bool* external = nullptr);

// <user folder>\texture-cache\x2
std::filesystem::path cacheRoot(const std::filesystem::path& userFolder);
// <root>\<game path>
std::filesystem::path cacheFile(const std::filesystem::path& root, const std::string& vfsPath);
// The cached upscale of vfsPath when it exists and was made from exactly these source bytes.
bool loadCached(const std::filesystem::path& root, const std::string& vfsPath, const std::vector<uint8_t>& source,
                render::Image& out);

// Signature of the install's archives (names, sizes, times): the cache's complete.txt records the one it was made for.
std::string installSignature(const Vfs& vfs);
bool cacheComplete(const std::filesystem::path& root, const std::string& signature);

struct GenerateOptions {
  std::string only;                 // only paths starting with this
  std::vector<std::string> first;   // made before the rest (the textures the game has loaded)
  std::string external;             // an external upscaler's command line, see texture-upscaling.md Â§5
  bool externalUiOnly = false;      // the external upscaler only for UI art (icons, cursors, loading and menu art)
  bool force = false;               // make every file again
  bool ui = true;                   // include UI art
  int threads = 0;                  // 0: all cores
  bool markComplete = true;         // write complete.txt at the end (not with `only`)
};

struct Progress {
  std::atomic<int> total{0}, done{0}, written{0}, upToDate{0}, skipped{0}, failed{0};
  std::atomic<uint64_t> bytesSource{0}, bytesCache{0};  // GPU bytes of the textures made: before / after
  std::atomic<bool> stop{false}, finished{false};
  std::mutex madeMutex;
  std::vector<std::string> made;  // game paths written so far (the game swaps them in), under madeMutex
};

// Makes the cache for every texture of the install. read is called under a lock (the VFS is not thread-safe).
bool generate(const Vfs& vfs, const std::filesystem::path& root, const GenerateOptions& opt, Progress& p,
              const std::function<void(const std::string&)>& log);

// PNG reader for the external upscaler's output (8-bit grey, grey+alpha, RGB, RGBA, palette; no interlace).
bool decodePng(const std::vector<uint8_t>& png, std::vector<uint8_t>& rgba, int& w, int& h);

}  // namespace regs::engine::texup
