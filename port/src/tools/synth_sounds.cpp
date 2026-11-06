// regs-tool synth <dir>: procedural sounds for the port's built-in mods, written at build time (CMakeLists regs_data)
// so no recorded audio ships: filtered noise, deterministic (fixed seeds). Currently the weather mod's
//   rain.wav     6 s stereo loop: hiss (band-limited noise, slow swell) plus scattered drop ticks, seamless
//   thunder.wav  7 s mono: a distant rumble (low-passed brown noise with a few swells and a long decay)
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kRate = 22050;
constexpr float kPi = 3.14159265f;

bool writeWav(const fs::path& file, const std::vector<float>& samples, int channels) {
  FILE* f = _wfopen(file.wstring().c_str(), L"wb");
  if (!f) return false;
  auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  uint32_t bytes = uint32_t(samples.size() * 2);
  std::fwrite("RIFF", 1, 4, f);
  u32(36 + bytes);
  std::fwrite("WAVEfmt ", 1, 8, f);
  u32(16);
  u16(1);
  u16(uint16_t(channels));
  u32(kRate);
  u32(uint32_t(kRate * channels * 2));
  u16(uint16_t(channels * 2));
  u16(16);
  std::fwrite("data", 1, 4, f);
  u32(bytes);
  for (float s : samples) {
    int16_t v = int16_t(std::clamp(int(std::lround(s * 32767.0f)), -32767, 32767));
    std::fwrite(&v, 2, 1, f);
  }
  return std::fclose(f) == 0;
}

struct OnePole {  // y += a·(x − y): low-pass with the cut-off fc
  float a = 1, y = 0;
  explicit OnePole(float fc) : a(1 - std::exp(-2 * kPi * fc / kRate)) {}
  float low(float x) { return y += a * (x - y); }
  float high(float x) { return x - low(x); }
};

void normalise(std::vector<float>& s, float peak) {
  float m = 0;
  for (float x : s) m = std::max(m, std::fabs(x));
  if (m > 0)
    for (float& x : s) x *= peak / m;
}

// one channel of rain: n samples plus the crossfade tail
std::vector<float> rainChannel(size_t n, uint32_t seed) {
  std::mt19937 rng(seed);
  std::uniform_real_distribution<float> uni(-1, 1), u01(0, 1);
  std::vector<float> out(n, 0);
  OnePole hp1(350), hp2(350), lp1(5200), lp2(7000), body(900);
  for (size_t i = 0; i < n; ++i) {
    float t = float(i) / kRate;
    float swell = 0.85f + 0.1f * std::sin(2 * kPi * t / 3.0f + float(seed % 7)) + 0.05f * std::sin(2 * kPi * t * 0.71f);
    float w = uni(rng);
    float hiss = lp2.low(lp1.low(hp2.high(hp1.high(w))));
    float low = body.low(uni(rng)) * 0.6f;  // a little body: the rain on roofs and streets
    out[i] = (hiss + low) * swell;
  }
  // drop ticks: short decaying noise bursts at random places and pitches
  size_t drops = n / 40;
  for (size_t d = 0; d < drops; ++d) {
    size_t at = size_t(u01(rng) * float(n));
    float amp = 0.15f + 0.6f * u01(rng) * u01(rng);
    float fc = 1500 + 4000 * u01(rng);
    OnePole lp(fc), hp(fc * 0.5f);
    size_t len = size_t(kRate * (0.004f + 0.01f * u01(rng)));
    for (size_t k = 0; k < len && at + k < n; ++k) {
      float env = std::exp(-6.0f * float(k) / float(len));
      out[at + k] += amp * env * hp.high(lp.low(uni(rng)));
    }
  }
  return out;
}

std::vector<float> rain(float seconds) {
  const size_t n = size_t(seconds * kRate), fade = size_t(0.5f * kRate);
  std::vector<float> l = rainChannel(n + fade, 7001), r = rainChannel(n + fade, 9157);
  // seamless loop: the tail after n is faded into the start (equal-power)
  for (auto* ch : {&l, &r}) {
    for (size_t k = 0; k < fade; ++k) {
      float t = float(k) / float(fade);
      (*ch)[k] = (*ch)[k] * std::sin(t * kPi / 2) + (*ch)[n + k] * std::cos(t * kPi / 2);
    }
    ch->resize(n);
  }
  std::vector<float> s(n * 2);
  for (size_t i = 0; i < n; ++i) s[2 * i] = l[i], s[2 * i + 1] = r[i];
  normalise(s, 0.7f);
  return s;
}

std::vector<float> thunder(float seconds) {
  const size_t n = size_t(seconds * kRate);
  std::mt19937 rng(4242);
  std::uniform_real_distribution<float> uni(-1, 1);
  std::vector<float> s(n);
  OnePole lp1(140), lp2(110), lp3(260), hp(25);
  float brown = 0;
  // swells: (start s, peak gain, rise s, decay s), a first roll and a few later ones, as a distant strike rolls
  const float swells[][4] = {{0.05f, 1.0f, 0.25f, 1.2f}, {0.7f, 0.8f, 0.3f, 1.6f}, {1.9f, 0.55f, 0.4f, 1.8f},
                             {3.2f, 0.35f, 0.5f, 2.2f}};
  for (size_t i = 0; i < n; ++i) {
    float t = float(i) / kRate;
    brown = std::clamp(brown * 0.995f + uni(rng) * 0.08f, -1.0f, 1.0f);
    float rumble = hp.high(lp2.low(lp1.low(brown)));
    float crackle = lp3.low(uni(rng)) * 0.25f;  // a softened crack in the first roll
    float env = 0, crackEnv = 0;
    for (const auto& w : swells) {
      if (t < w[0]) continue;
      float u = t - w[0];
      float e = w[1] * (u < w[2] ? u / w[2] : std::exp(-(u - w[2]) / w[3]));
      env += e;
      if (&w == &swells[0]) crackEnv = e * std::exp(-u * 3.0f);
    }
    float tail = t > seconds - 0.6f ? (seconds - t) / 0.6f : 1.0f;  // ends at silence
    s[i] = (rumble * env + crackle * crackEnv) * tail;
  }
  normalise(s, 0.9f);
  return s;
}

}  // namespace

int cmdSynth(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: regs-tool synth <dir>   (writes rain.wav and thunder.wav)\n");
    return 1;
  }
  fs::path dir = fs::path(argv[2]);
  std::error_code ec;
  fs::create_directories(dir, ec);
  bool ok = writeWav(dir / "rain.wav", rain(6.0f), 2) && writeWav(dir / "thunder.wav", thunder(7.0f), 1);
  std::printf("synth: %s %s\n", ok ? "wrote" : "FAILED writing", (dir / "{rain,thunder}.wav").string().c_str());
  return ok ? 0 : 1;
}
