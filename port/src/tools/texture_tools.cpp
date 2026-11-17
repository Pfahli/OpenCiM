// regs-tool commands for the upscaled-texture cache (docs/renderer/texture-upscaling.md):
//   regs-tool upscale-textures <gameDir> <cacheDir> [--only <prefix>] [--external "<cmd {in} {out}>"] [--force]
//                              [--no-ui] [--threads <n>]
//   regs-tool upscale-compare <gameDir> <path> <out.png> [x y size]   GPU-style bilinear 2× | cached upscale, side by side
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "core/text.h"
#include "engine/texture_upscale.h"
#include "render/image.h"
#include "vfs/vfs.h"

using namespace regs;
namespace fs = std::filesystem;
namespace texup = regs::engine::texup;

namespace {

fs::path widenArg(const char* s) {
  auto w = utf8ToUtf16(s);
  return fs::path(std::wstring(w.begin(), w.end()));
}

// the files the game sees: the official archives only, as the engine mounts them (engine.cpp)
void openInstall(Vfs& vfs, const char* gameDir) {
  vfs.setInstallRoot(widenArg(gameDir));
  vfs.rescan();
}

bool writeBytes(const fs::path& p, const std::vector<uint8_t>& d) {
  FILE* f = _wfopen(p.wstring().c_str(), L"wb");
  if (!f) return false;
  std::fwrite(d.data(), 1, d.size(), f);
  std::fclose(f);
  return true;
}

}  // namespace

int cmdUpscaleTextures(int argc, char** argv) {
  if (argc < 4) return 2;
  Vfs vfs;
  openInstall(vfs, argv[2]);
  texup::GenerateOptions opt;
  for (int i = 4; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--only" && i + 1 < argc) opt.only = argv[++i];
    else if (a == "--external" && i + 1 < argc) opt.external = argv[++i];
    else if (a == "--force") opt.force = true;
    else if (a == "--no-ui") opt.ui = false;
    else if (a == "--threads" && i + 1 < argc) opt.threads = std::atoi(argv[++i]);
    else return 2;
  }
  texup::Progress p;
  auto t0 = std::chrono::steady_clock::now();
  std::thread report([&] {
    while (!p.finished) {
      std::this_thread::sleep_for(std::chrono::seconds(5));
      if (!p.finished)
        std::printf("  %d / %d (%d made, %d up to date, %d skipped, %d failed)\n", p.done.load(), p.total.load(),
                    p.written.load(), p.upToDate.load(), p.skipped.load(), p.failed.load());
    }
  });
  bool ok = texup::generate(vfs, widenArg(argv[3]), opt, p, [](const std::string& s) { std::printf("%s\n", s.c_str()); });
  report.join();
  double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("%d textures: %d made, %d up to date, %d skipped, %d failed in %.1f s; GPU bytes of the made ones %.1f MB -> %.1f MB\n",
              p.total.load(), p.written.load(), p.upToDate.load(), p.skipped.load(), p.failed.load(), s,
              double(p.bytesSource.load()) / 1048576.0, double(p.bytesCache.load()) / 1048576.0);
  return ok ? 0 : 1;
}

int cmdUpscaleCompare(int argc, char** argv) {
  if (argc < 5) return 2;
  Vfs vfs;
  openInstall(vfs, argv[2]);
  std::vector<uint8_t> data;
  render::Image src;
  if (!vfs.read(argv[3], data) || !render::parseDds(data, src, nullptr)) {
    std::fprintf(stderr, "%s: cannot read\n", argv[3]);
    return 1;
  }
  auto read = [&](const std::string& p, std::vector<uint8_t>& out) { return vfs.read(p, out); };
  texup::Roles roles;  // the folder rules are enough for a look at one texture
  texup::Kind kind = texup::classify(argv[3], roles, src);
  if (kind == texup::Kind::Skip) kind = texup::Kind::Color;
  std::vector<uint8_t> up;
  int w2 = 0, h2 = 0;
  if (!texup::upscaleLevel0(src, kind, kind != texup::Kind::Ui, up, w2, h2)) return 1;
  // through the encoder, as the game will see it
  render::Image res;
  std::vector<uint8_t> enc;
  int ew = 0, eh = 0;
  if (!texup::buildUpscaled(src, up, res) || !render::decodeRgba(res, 0, enc, ew, eh)) return 1;
  std::vector<uint8_t> orig;
  int w = 0, h = 0;
  render::decodeRgba(src, 0, orig, w, h);
  // what the GPU does with the original when it is magnified 2×: bilinear
  std::vector<uint8_t> bil(size_t(w2) * size_t(h2) * 4);
  for (int y = 0; y < h2; ++y)
    for (int x = 0; x < w2; ++x) {
      float sx = (x + 0.5f) / 2 - 0.5f, sy = (y + 0.5f) / 2 - 0.5f;
      int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
      float fx = sx - x0, fy = sy - y0;
      for (int c = 0; c < 4; ++c) {
        auto px = [&](int xx, int yy) {
          xx = ((xx % w) + w) % w, yy = ((yy % h) + h) % h;
          return float(orig[(size_t(yy) * size_t(w) + size_t(xx)) * 4 + size_t(c)]);
        };
        float v = (px(x0, y0) * (1 - fx) + px(x0 + 1, y0) * fx) * (1 - fy) + (px(x0, y0 + 1) * (1 - fx) + px(x0 + 1, y0 + 1) * fx) * fy;
        bil[(size_t(y) * size_t(w2) + size_t(x)) * 4 + size_t(c)] = uint8_t(v + 0.5f);
      }
    }
  int cx = 0, cy = 0, cs = std::min(w2, h2);
  if (argc > 7) cx = std::atoi(argv[5]) * 2, cy = std::atoi(argv[6]) * 2, cs = std::atoi(argv[7]) * 2;
  cs = std::min({cs, w2 - cx, h2 - cy});
  // crop, each side shown at 2× again (nearest) so the texels are visible on a phone
  const int z = 2, W = cs * z * 2 + 8, H = cs * z;
  std::vector<uint8_t> out(size_t(W) * size_t(H) * 4, 255);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < cs * z; ++x) {
      size_t s = (size_t(cy + y / z) * size_t(w2) + size_t(cx + x / z)) * 4;
      for (int c = 0; c < 3; ++c) {
        out[(size_t(y) * size_t(W) + size_t(x)) * 4 + size_t(c)] = bil[s + size_t(c)];
        out[(size_t(y) * size_t(W) + size_t(x + cs * z + 8)) * 4 + size_t(c)] = enc[s + size_t(c)];
      }
    }
  if (!writeBytes(widenArg(argv[4]), render::encodePng(out.data(), W, H))) return 1;
  std::printf("%s: %dx%d %s, %s -> %dx%d (%d levels)\n", argv[3], w, h, texup::kindName(kind),
              src.compressed() ? "DXT" : "raw", res.width, res.height, res.mipCount);
  (void)read;
  return 0;
}

// png-roundtrip <in.png> <out.png>: the PNG reader on a file another program wrote (a check of decodePng)
int cmdPngRoundtrip(int argc, char** argv) {
  if (argc < 4) return 2;
  FILE* f = _wfopen(widenArg(argv[2]).wstring().c_str(), L"rb");
  if (!f) return 1;
  std::vector<uint8_t> d;
  uint8_t buf[65536];
  for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
  std::fclose(f);
  std::vector<uint8_t> rgba;
  int w = 0, h = 0;
  if (!texup::decodePng(d, rgba, w, h)) {
    std::fprintf(stderr, "cannot decode\n");
    return 1;
  }
  std::printf("%dx%d\n", w, h);
  return writeBytes(widenArg(argv[3]), render::encodePng(rgba.data(), w, h)) ? 0 : 1;
}

// png-diff <a.png> <b.png> [x y w h]: share of differing pixels and mean absolute difference (0..255) in a region
int cmdPngDiff(int argc, char** argv) {
  if (argc < 4) return 2;
  auto load = [](const char* p, std::vector<uint8_t>& rgba, int& w, int& h) {
    FILE* f = _wfopen(widenArg(p).wstring().c_str(), L"rb");
    if (!f) return false;
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    return texup::decodePng(d, rgba, w, h);
  };
  std::vector<uint8_t> a, b;
  int aw = 0, ah = 0, bw = 0, bh = 0;
  if (!load(argv[2], a, aw, ah) || !load(argv[3], b, bw, bh) || aw != bw || ah != bh) {
    std::fprintf(stderr, "cannot read or sizes differ\n");
    return 1;
  }
  int x0 = 0, y0 = 0, w = aw, h = ah;
  if (argc > 7) x0 = std::atoi(argv[4]), y0 = std::atoi(argv[5]), w = std::atoi(argv[6]), h = std::atoi(argv[7]);
  size_t n = 0, diff = 0;
  double sum = 0, ma[3] = {0, 0, 0}, mb[3] = {0, 0, 0};
  for (int y = y0; y < y0 + h && y < ah; ++y)
    for (int x = x0; x < x0 + w && x < aw; ++x) {
      size_t i = (size_t(y) * size_t(aw) + size_t(x)) * 4;
      int e = std::abs(a[i] - b[i]) + std::abs(a[i + 1] - b[i + 1]) + std::abs(a[i + 2] - b[i + 2]);
      ++n;
      diff += e > 0;
      sum += e / 3.0;
      for (int c = 0; c < 3; ++c) ma[c] += a[i + size_t(c)], mb[c] += b[i + size_t(c)];
    }
  std::printf("%zu of %zu pixels differ (%.2f %%), mean abs difference %.3f\n", diff, n, 100.0 * double(diff) / double(n), sum / double(n));
  std::printf("mean RGB a %.1f %.1f %.1f, b %.1f %.1f %.1f\n", ma[0] / double(n), ma[1] / double(n), ma[2] / double(n),
              mb[0] / double(n), mb[1] / double(n), mb[2] / double(n));
  return 0;
}

// upscale-check <gameDir> <cacheDir> [prefix]: every cached texture against its source: the 2× level boxed back down
// should match the source's level 0 per channel (mean absolute difference, 0..255); the worst ones are listed
int cmdUpscaleCheck(int argc, char** argv) {
  if (argc < 4) return 2;
  Vfs vfs;
  openInstall(vfs, argv[2]);
  std::string prefix = argc > 4 ? argv[4] : "";
  struct Row {
    double d[4];
    std::string path;
  };
  std::vector<Row> rows;
  for (const auto& [path, rec] : vfs.records()) {
    if (path.compare(0, prefix.size(), prefix) != 0 || path.size() < 4) continue;
    std::vector<uint8_t> data;
    render::Image src, up;
    if (!vfs.read(path, data) || !texup::loadCached(widenArg(argv[3]), path, data, up) || !render::parseDds(data, src, nullptr)) continue;
    std::vector<uint8_t> a, b;
    int w = 0, h = 0, w2 = 0, h2 = 0;
    if (!render::decodeRgba(src, 0, a, w, h) || !render::decodeRgba(up, 0, b, w2, h2) || w2 != 2 * w || h2 != 2 * h) continue;
    Row r{{0, 0, 0, 0}, path};
    bool ui = texup::classify(path, texup::Roles(), src) == texup::Kind::Ui;  // colour under alpha 0 is invisible there
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < w; ++x)
        for (int c = 0; c < 4; ++c) {
          int s = 0;
          for (int j = 0; j < 2; ++j)
            for (int i = 0; i < 2; ++i) s += b[(size_t(2 * y + j) * size_t(w2) + size_t(2 * x + i)) * 4 + size_t(c)];
          r.d[c] += std::abs(s / 4.0 - a[(size_t(y) * size_t(w) + size_t(x)) * 4 + size_t(c)]) *
                    (ui && c < 3 ? a[(size_t(y) * size_t(w) + size_t(x)) * 4 + 3] / 255.0 : 1.0);
        }
    for (double& v : r.d) v /= double(w) * double(h);
    rows.push_back(r);
  }
  std::sort(rows.begin(), rows.end(), [](const Row& x, const Row& y) {
    return std::max({x.d[0], x.d[1], x.d[2], x.d[3]}) > std::max({y.d[0], y.d[1], y.d[2], y.d[3]});
  });
  double sum[4] = {0, 0, 0, 0};
  for (const Row& r : rows)
    for (int c = 0; c < 4; ++c) sum[c] += r.d[c];
  std::printf("%zu cached textures; mean difference R %.2f G %.2f B %.2f A %.2f\n", rows.size(), sum[0] / double(rows.size()),
              sum[1] / double(rows.size()), sum[2] / double(rows.size()), sum[3] / double(rows.size()));
  for (size_t i = 0; i < rows.size() && i < 25; ++i)
    std::printf("  %6.2f %6.2f %6.2f %6.2f  %s\n", rows[i].d[0], rows[i].d[1], rows[i].d[2], rows[i].d[3], rows[i].path.c_str());
  return 0;
}

// ddspng <file.dds> <out.png> [level]: one level of a DDS file on disk (a cache file) as PNG
int cmdDdsPng(int argc, char** argv) {
  if (argc < 4) return 2;
  FILE* f = _wfopen(widenArg(argv[2]).wstring().c_str(), L"rb");
  if (!f) return 1;
  std::vector<uint8_t> d;
  uint8_t buf[65536];
  for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
  std::fclose(f);
  render::Image img;
  std::vector<uint8_t> rgba;
  int w = 0, h = 0;
  if (!render::parseDds(d, img, nullptr) || !render::decodeRgba(img, argc > 4 ? std::atoi(argv[4]) : 0, rgba, w, h)) return 1;
  for (size_t i = 3; i < rgba.size(); i += 4) rgba[i] = 255;  // colour only
  std::printf("%dx%d, %d levels\n", w, h, img.mipCount);
  return writeBytes(widenArg(argv[3]), render::encodePng(rgba.data(), w, h)) ? 0 : 1;
}
