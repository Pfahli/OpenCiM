// Asset makers for the port's built-in mods, run by the build (CMakeLists regs_data).
//
// regs-tool cut <in> <out.wav> <start s> <length s> <fade s> [loop]: cuts a piece of a sound file (WAV, MP3, FLAC;
// decoded by miniaudio as 44.1 kHz stereo) into a 16-bit WAV. With "loop" the piece is made seamless: the fade
// seconds after its end are crossfaded (equal power) into its start, so it repeats without a click or gap (MP3 files
// carry encoder padding, which a plain loop of the file would play as a gap). Without, the end fades out over fade s.
// The build makes the weather mod's loops with it from the mod's own CC0 recordings (CMakeLists regs_data).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "miniaudio.h"

namespace fs = std::filesystem;

namespace {

constexpr int kRate = 44100, kChannels = 2;

bool readFile(const fs::path& p, std::vector<uint8_t>& out) {
  FILE* f = _wfopen(p.wstring().c_str(), L"rb");
  if (!f) return false;
  uint8_t buf[65536];
  for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) out.insert(out.end(), buf, buf + n);
  std::fclose(f);
  return true;
}

bool writeWav(const fs::path& file, const std::vector<float>& s) {
  FILE* f = _wfopen(file.wstring().c_str(), L"wb");
  if (!f) return false;
  auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  uint32_t bytes = uint32_t(s.size() * 2);
  std::fwrite("RIFF", 1, 4, f);
  u32(36 + bytes);
  std::fwrite("WAVEfmt ", 1, 8, f);
  u32(16);
  u16(1);
  u16(kChannels);
  u32(kRate);
  u32(kRate * kChannels * 2);
  u16(kChannels * 2);
  u16(16);
  std::fwrite("data", 1, 4, f);
  u32(bytes);
  std::vector<int16_t> pcm(s.size());
  for (size_t i = 0; i < s.size(); ++i) pcm[i] = int16_t(std::clamp(int(std::lround(s[i] * 32767.0f)), -32767, 32767));
  std::fwrite(pcm.data(), 2, pcm.size(), f);
  return std::fclose(f) == 0;
}

}  // namespace

int cmdCut(int argc, char** argv) {
  if (argc < 7) {
    std::fprintf(stderr, "usage: regs-tool cut <in> <out.wav> <start s> <length s> <fade s> [loop]\n");
    return 1;
  }
  fs::path in = argv[2], out = argv[3];
  double start = std::atof(argv[4]), length = std::atof(argv[5]), fade = std::atof(argv[6]);
  bool loop = argc > 7 && std::string(argv[7]) == "loop";
  std::vector<uint8_t> data;
  if (!readFile(in, data)) {
    std::fprintf(stderr, "cut: cannot read %s\n", in.string().c_str());
    return 1;
  }
  ma_decoder_config cfg = ma_decoder_config_init(ma_format_f32, kChannels, kRate);
  ma_uint64 frames = 0;
  void* pcm = nullptr;
  if (ma_decode_memory(data.data(), data.size(), &cfg, &frames, &pcm) != MA_SUCCESS || !pcm) {
    std::fprintf(stderr, "cut: cannot decode %s\n", in.string().c_str());
    return 1;
  }
  const float* src = static_cast<const float*>(pcm);
  size_t first = size_t(start * kRate), n = size_t(length * kRate), f = size_t(fade * kRate);
  size_t need = first + n + (loop ? f : 0);
  if (need > frames) {  // shorter than asked: take what there is
    first = std::min<size_t>(first, size_t(frames) / 4);
    n = size_t(frames) - first - (loop ? std::min<size_t>(f, size_t(frames) / 4) : 0);
    f = std::min(f, n / 2);
  }
  std::vector<float> s(n * kChannels);
  for (size_t i = 0; i < n * kChannels; ++i) s[i] = src[first * kChannels + i];
  if (loop) {
    for (size_t k = 0; k < f; ++k) {
      float t = float(k) / float(f), a = std::sin(t * 1.5707963f), b = std::cos(t * 1.5707963f);
      for (int c = 0; c < kChannels; ++c)
        s[k * kChannels + size_t(c)] = s[k * kChannels + size_t(c)] * a + src[(first + n + k) * kChannels + size_t(c)] * b;
    }
  } else {
    for (size_t k = 0; k < f && k < n; ++k) {
      float g = float(k) / float(f);
      for (int c = 0; c < kChannels; ++c) s[(n - 1 - k) * kChannels + size_t(c)] *= g;
    }
  }
  ma_free(pcm, nullptr);
  std::error_code ec;
  fs::create_directories(out.parent_path(), ec);
  bool ok = writeWav(out, s);
  std::printf("cut: %s %s (%.1f s from %.1f s%s)\n", ok ? "wrote" : "FAILED", out.string().c_str(), double(n) / kRate,
              double(first) / kRate, loop ? ", loop" : "");
  return ok ? 0 : 1;
}

// regs-tool softdot <out.dds> <size>: a white sprite whose alpha falls off smoothly from the centre to zero at the
// edge ((1 - r²)², no visible outline), uncompressed 32-bit DDS with mipmaps: the weather mod's rain and spray particles
int cmdSoftDot(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: regs-tool softdot <out.dds> <size>\n");
    return 1;
  }
  fs::path out = argv[2];
  int size = std::clamp(std::atoi(argv[3]), 4, 256);
  int mips = 0;
  for (int s = size; s >= 1; s >>= 1) ++mips;
  std::vector<uint8_t> file(128, 0);
  auto put = [&](size_t at, uint32_t v) {
    for (int k = 0; k < 4; ++k) file[at + size_t(k)] = uint8_t(v >> (8 * k));
  };
  file[0] = 'D', file[1] = 'D', file[2] = 'S', file[3] = ' ';
  put(4, 124);                                 // header size
  put(8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000);  // CAPS | HEIGHT | WIDTH | PIXELFORMAT | MIPMAPCOUNT
  put(12, uint32_t(size));
  put(16, uint32_t(size));
  put(28, uint32_t(mips));
  put(76, 32);                                 // pixel format size
  put(80, 0x40 | 0x1);                         // RGB | ALPHAPIXELS
  put(88, 32);
  put(92, 0x00FF0000), put(96, 0x0000FF00), put(100, 0x000000FF), put(104, 0xFF000000);
  put(108, 0x1000 | 0x400000 | 0x8);           // TEXTURE | MIPMAP | COMPLEX
  for (int s = size; s >= 1; s >>= 1)
    for (int y = 0; y < s; ++y)
      for (int x = 0; x < s; ++x) {
        float dx = (float(x) + 0.5f) / float(s) * 2 - 1, dy = (float(y) + 0.5f) / float(s) * 2 - 1;
        float r2 = std::min(1.0f, dx * dx + dy * dy), a = (1 - r2) * (1 - r2);
        if (s == 1) a = 0.25f;  // about the mean of the full-size sprite
        file.insert(file.end(), {255, 255, 255, uint8_t(std::lround(a * 255))});
      }
  std::error_code ec;
  fs::create_directories(out.parent_path(), ec);
  FILE* f = _wfopen(out.wstring().c_str(), L"wb");
  bool ok = f && std::fwrite(file.data(), 1, file.size(), f) == file.size();
  if (f) std::fclose(f);
  std::printf("softdot: %s %s (%d px, %d mips)\n", ok ? "wrote" : "FAILED", out.string().c_str(), size, mips);
  return ok ? 0 : 1;
}

// regs-tool leaf <out.dds> <size>: the weather mod's falling-leaf sprite, a white (tinted by the particle colour)
// pointed leaf blade along the sprite's x axis with a short stem, a slightly darker midrib and a crisp, antialiased
// edge (no soft glow like the rain's dot); its mip levels are box-filtered from the full size.
int cmdLeaf(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: regs-tool leaf <out.dds> <size>\n");
    return 1;
  }
  fs::path out = argv[2];
  int size = std::clamp(std::atoi(argv[3]), 8, 256);
  size = 1 << int(std::lround(std::log2(double(size))));
  int mips = 0;
  for (int s = size; s >= 1; s >>= 1) ++mips;
  std::vector<float> rgba(size_t(size) * size * 4);
  const float px = 2.0f / float(size);
  for (int y = 0; y < size; ++y)
    for (int x = 0; x < size; ++x) {
      float u = (float(x) + 0.5f) * px - 1, v = (float(y) + 0.5f) * px - 1;
      // blade from u = -0.75 (base) to 0.9 (tip), widest a third of the way, pointed at the tip
      float t = (u + 0.75f) / 1.65f;
      float w = t > 0 && t < 1 ? 0.42f * std::pow(std::sin(3.14159265f * std::pow(t, 0.8f)), 0.9f) : 0.0f;
      float blade = std::clamp((w - std::fabs(v)) / px + 0.5f, 0.0f, 1.0f);
      float stem = (u > -0.95f && u < -0.7f) ? std::clamp((0.05f - std::fabs(v)) / px + 0.5f, 0.0f, 1.0f) : 0.0f;
      float a = std::max(blade, stem);
      float shade = 1.0f - 0.18f * std::exp(-v * v / 0.004f) - 0.1f * std::clamp(std::fabs(v) / std::max(w, 0.05f), 0.0f, 1.0f);
      float* p = &rgba[(size_t(y) * size + x) * 4];
      p[0] = p[1] = p[2] = shade, p[3] = a;
    }
  std::vector<uint8_t> file(128, 0);
  auto put = [&](size_t at, uint32_t val) {
    for (int k = 0; k < 4; ++k) file[at + size_t(k)] = uint8_t(val >> (8 * k));
  };
  file[0] = 'D', file[1] = 'D', file[2] = 'S', file[3] = ' ';
  put(4, 124);
  put(8, 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000);
  put(12, uint32_t(size));
  put(16, uint32_t(size));
  put(28, uint32_t(mips));
  put(76, 32);
  put(80, 0x40 | 0x1);
  put(88, 32);
  put(92, 0x00FF0000), put(96, 0x0000FF00), put(100, 0x000000FF), put(104, 0xFF000000);
  put(108, 0x1000 | 0x400000 | 0x8);
  std::vector<float> level = rgba;
  for (int s = size; s >= 1; s >>= 1) {
    for (int i = 0; i < s * s; ++i) {
      const float* p = &level[size_t(i) * 4];
      float a = p[3], c = a > 0 ? p[0] : 1.0f;  // colour weighted by coverage below, kept where transparent
      file.insert(file.end(), {uint8_t(std::lround(c * 255)), uint8_t(std::lround(c * 255)), uint8_t(std::lround(c * 255)),
                               uint8_t(std::lround(a * 255))});
    }
    if (s == 1) break;
    int h = s / 2;
    std::vector<float> next(size_t(h) * h * 4, 0.0f);
    for (int y = 0; y < h; ++y)
      for (int x = 0; x < h; ++x) {
        float* q = &next[(size_t(y) * h + x) * 4];
        float sa = 0, sc = 0;
        for (int k = 0; k < 4; ++k) {
          const float* p = &level[(size_t(2 * y + (k >> 1)) * s + size_t(2 * x + (k & 1))) * 4];
          sa += p[3], sc += p[0] * p[3];
        }
        q[3] = sa / 4, q[0] = q[1] = q[2] = sa > 0 ? sc / sa : 1.0f;
      }
    level.swap(next);
  }
  std::error_code ec;
  fs::create_directories(out.parent_path(), ec);
  FILE* f = _wfopen(out.wstring().c_str(), L"wb");
  bool ok = f && std::fwrite(file.data(), 1, file.size(), f) == file.size();
  if (f) std::fclose(f);
  std::printf("leaf: %s %s (%d px, %d mips)\n", ok ? "wrote" : "FAILED", out.string().c_str(), size, mips);
  return ok ? 0 : 1;
}
