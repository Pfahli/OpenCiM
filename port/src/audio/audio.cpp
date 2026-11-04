#include "audio/audio.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>

#include "core/log.h"
#include "miniaudio.h"

namespace regs::audio {

struct Clip {
  ClipInfo info;
  std::vector<float> pcm;      // decoded effects: interleaved f32 frames
  std::vector<uint8_t> bytes;  // streams: the encoded file, decoded per voice while playing
};

namespace {

constexpr int kMaxCh = 8;          // bus channels (7.1)
constexpr int kMaxMixed = 64;      // FMOD Ex's software voices (§4.2); the quietest beyond are virtual
constexpr float kSpeedOfSound = 340.0f;  // FMOD Ex's default (units/s, distance factor 1)
constexpr int kHist = 64;          // headphones: the delay line of a voice (> 0.66 ms at 96 kHz)
constexpr float kPi = 3.14159265f;

const char* const kLayoutNames[] = {"Default", "Mono", "Stereo", "Quad", "Surround", "5p1", "7p1", "Prologic", "Headphones"};

struct Voice {
  int id = 0;
  ClipRef clip;
  // ---- set by the main thread (under the lock) ----
  float volume = 1, pitch = 1;
  bool loop = false, paused = false, stopped = false, spatial = false, relative = false;
  float pos[3] = {}, vel[3] = {};
  float minD = 10, maxD = 100;
  // ---- computed by update() ----
  float target[2][kMaxCh] = {};  // gain per source row (0: mono / left, 1: right) and bus channel
  bool stereoRows = false;       // the source's two channels go separately (2D pass-through); else downmixed
  float rate = 1;                // source frames per output frame
  bool virt = false;
  float audibility = 0;
  float rel[3] = {}, dist = 0;   // listener space, for the debug log
  bool binaural = false;         // headphones: per-ear delay and low-pass
  float itdTarget[2] = {}, lpTarget[2] = {1, 1};
  // ---- audio thread ----
  float cur[2][kMaxCh] = {};
  bool started = false;
  double frame = 0;  // source frame position (PCM: wrapped for loops; streams: logical, increasing)
  bool finished = false;
  // streams
  ma_decoder dec{};
  bool hasDec = false, decEof = false;
  std::vector<float> buf;
  uint64_t bufStart = 0;
  uint32_t bufLen = 0;
  // headphones
  float hist[kHist] = {};
  int histPos = 0;
  float itd[2] = {}, lp[2] = {1, 1}, lpState[2] = {};
  ~Voice() {
    if (hasDec) ma_decoder_uninit(&dec);
  }
};

struct Speaker {
  int channel = 0;
  float azimuth = 0;  // radians, 0 = front, + = right
};

struct State {
  std::mutex lock;  // voices and the listener: the main thread writes, the audio thread mixes
  bool open = false;
  ma_context context{};
  bool hasContext = false;
  ma_device device{};
  Layout layout = Layout::Default;
  uint32_t outCh = 2, busCh = 2, rate = 48000;
  ma_channel map[kMaxCh] = {};  // bus channel map (Prologic: the 5.0 bus before encoding)
  std::vector<Speaker> speakers;  // spatial bus channels sorted by azimuth (VBAP over 3+)
  int frontL = 0, frontR = 1;   // bus channels for 2D stereo
  int lfe = -1;
  std::map<int, std::unique_ptr<Voice>> voices;
  int nextId = 1;
  float master = 1;
  float lpos[3] = {}, lfwd[3] = {0, 0, -1}, lup[3] = {0, 1, 0}, lvel[3] = {};
  std::vector<float> bus;
  // stats (audio thread accumulates)
  double sumSq[kMaxCh] = {};
  uint64_t statFrames = 0;
  int mixedCount = 0, virtualCount = 0, pausedCount = 0;
  // capture
  std::mutex captureLock;
  std::vector<int16_t> captureQueue;
  FILE* capture = nullptr;
  uint64_t captureBytes = 0;
};

State& st() {
  static State s;
  return s;
}

const char* channelName(ma_channel c) {
  switch (c) {
    case MA_CHANNEL_MONO: return "M";
    case MA_CHANNEL_FRONT_LEFT: return "FL";
    case MA_CHANNEL_FRONT_RIGHT: return "FR";
    case MA_CHANNEL_FRONT_CENTER: return "FC";
    case MA_CHANNEL_LFE: return "LFE";
    case MA_CHANNEL_BACK_LEFT: return "BL";
    case MA_CHANNEL_BACK_RIGHT: return "BR";
    case MA_CHANNEL_BACK_CENTER: return "BC";
    case MA_CHANNEL_SIDE_LEFT: return "SL";
    case MA_CHANNEL_SIDE_RIGHT: return "SR";
    case MA_CHANNEL_FRONT_LEFT_CENTER: return "FLC";
    case MA_CHANNEL_FRONT_RIGHT_CENTER: return "FRC";
    default: return "?";
  }
}

// The speaker table of a bus channel map: azimuths of the horizontal channels (ITU-style: fronts ±30°, centre 0,
// sides ±110° in 5.x and ±90° in 7.1, backs ±110° without sides and ±150° with them; quad ±45°/±135°).
void buildSpeakers(State& s) {
  s.speakers.clear();
  s.lfe = -1;
  bool sides = false, backs = false;
  for (uint32_t c = 0; c < s.busCh; ++c) {
    sides |= s.map[c] == MA_CHANNEL_SIDE_LEFT || s.map[c] == MA_CHANNEL_SIDE_RIGHT;
    backs |= s.map[c] == MA_CHANNEL_BACK_LEFT || s.map[c] == MA_CHANNEL_BACK_RIGHT;
  }
  bool quad = s.busCh == 4 && backs && !sides;
  auto deg = [](float d) { return d * kPi / 180.0f; };
  s.frontL = s.frontR = -1;
  for (uint32_t c = 0; c < s.busCh; ++c) {
    float az = 0;
    bool spatial = true;
    switch (s.map[c]) {
      case MA_CHANNEL_FRONT_LEFT: az = deg(quad ? -45.f : -30.f), s.frontL = int(c); break;
      case MA_CHANNEL_FRONT_RIGHT: az = deg(quad ? 45.f : 30.f), s.frontR = int(c); break;
      case MA_CHANNEL_FRONT_CENTER: az = 0; break;
      case MA_CHANNEL_FRONT_LEFT_CENTER: az = deg(-15); break;
      case MA_CHANNEL_FRONT_RIGHT_CENTER: az = deg(15); break;
      case MA_CHANNEL_SIDE_LEFT: az = deg(backs ? -90.f : -110.f); break;
      case MA_CHANNEL_SIDE_RIGHT: az = deg(backs ? 90.f : 110.f); break;
      case MA_CHANNEL_BACK_LEFT: az = deg(quad ? -135.f : sides ? -150.f : -110.f); break;
      case MA_CHANNEL_BACK_RIGHT: az = deg(quad ? 135.f : sides ? 150.f : 110.f); break;
      case MA_CHANNEL_BACK_CENTER: az = kPi; break;
      case MA_CHANNEL_LFE: s.lfe = int(c), spatial = false; break;
      default: spatial = false; break;  // mono, height and aux channels get no 3D pan
    }
    if (spatial) s.speakers.push_back({int(c), az});
  }
  std::sort(s.speakers.begin(), s.speakers.end(), [](const Speaker& a, const Speaker& b) { return a.azimuth < b.azimuth; });
  if (s.frontL < 0 || s.frontR < 0) {  // no front pair: the first two channels
    s.frontL = 0;
    s.frontR = s.busCh > 1 ? 1 : 0;
  }
}

// Pan gains (constant power) of a direction in listener space (x right, y up, −z forward) for the bus. d = 0: the
// listener itself (centre / front pair).
void panGains(const State& s, float x, float y, float z, float d, float out[kMaxCh]) {
  for (int c = 0; c < kMaxCh; ++c) out[c] = 0;
  (void)y;
  if (s.busCh == 1) {
    out[0] = 1;
    return;
  }
  if (s.speakers.size() <= 2) {  // stereo (§16.3): the lateral component, front and back alike like FMOD's stereo
    float p = d > 1e-3f ? std::clamp(x / d, -1.0f, 1.0f) : 0.0f;
    float a = (std::asin(p) + kPi / 2) / 2;
    out[s.frontL] = std::cos(a);
    out[s.frontR] = std::sin(a);
    return;
  }
  // 3+ speakers: pairwise constant-power (2D VBAP) between the two speakers around the azimuth, blended towards an even
  // spread when the source is above or below the listener
  // h: how horizontal the direction is; up to ~45° above or below the horizontal plane the pan is pure VBAP, steeper
  // directions fade to the spread
  float h = d > 1e-3f ? std::min(1.0f, std::sqrt(x * x + z * z) / d / 0.7071f) : 0.0f;
  if (d <= 1e-3f) {  // at the listener: the front pair, like 2D sounds
    out[s.frontL] = out[s.frontR] = 0.70710678f;
    return;
  }
  float az = std::atan2(x, -z);
  size_t n = s.speakers.size();
  size_t hi = 0;
  while (hi < n && s.speakers[hi].azimuth < az) ++hi;
  const Speaker& b = s.speakers[hi % n];
  const Speaker& a = s.speakers[(hi + n - 1) % n];
  float span = b.azimuth - a.azimuth, off = az - a.azimuth;
  if (span <= 0) span += 2 * kPi;
  if (off < 0) off += 2 * kPi;
  float t = std::clamp(off / span, 0.0f, 1.0f);
  float omni = 1.0f / std::sqrt(float(n));
  float h2 = h * h;
  for (const Speaker& sp : s.speakers) {
    float g = sp.channel == a.channel ? std::cos(t * kPi / 2) : sp.channel == b.channel ? std::sin(t * kPi / 2) : 0.0f;
    out[sp.channel] = std::sqrt(h2 * g * g + (1 - h2) * omni * omni);
  }
}

// Main thread, under the lock: the targets of one voice from its parameters and the listener.
void computeVoice(State& s, Voice& v) {
  const ClipInfo& ci = v.clip->info;
  float vol = std::clamp(v.volume, 0.0f, 1.0f);
  float doppler = 1;
  for (auto& row : v.target)
    for (float& g : row) g = 0;
  v.binaural = false;
  v.stereoRows = false;
  bool stereo = ci.channels >= 2;
  if (!v.spatial) {
    // 2D: unattenuated, unpanned; stereo to the front pair, mono centred (both front channels −3 dB)
    if (s.busCh == 1) {
      v.target[0][0] = vol * (stereo ? 0.5f : 1.0f);
      if (stereo) v.target[1][0] = vol * 0.5f, v.stereoRows = true;
    } else if (stereo) {
      v.target[0][s.frontL] = vol;
      v.target[1][s.frontR] = vol;
      v.stereoRows = true;
    } else {
      v.target[0][s.frontL] = v.target[0][s.frontR] = vol * 0.70710678f;
    }
    v.audibility = vol;
  } else {
    float rx, ry, rz;
    if (v.relative) {
      rx = v.pos[0], ry = v.pos[1], rz = v.pos[2];
    } else {
      float dx = v.pos[0] - s.lpos[0], dy = v.pos[1] - s.lpos[1], dz = v.pos[2] - s.lpos[2];
      // listener basis: forward f, up u (made orthogonal), right r = f × u
      const float* f = s.lfwd;
      float u[3] = {s.lup[0], s.lup[1], s.lup[2]};
      float fu = f[0] * u[0] + f[1] * u[1] + f[2] * u[2];
      for (int k = 0; k < 3; ++k) u[k] -= fu * f[k];
      float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
      if (ul > 1e-6f)
        for (float& c : u) c /= ul;
      float r[3] = {f[1] * u[2] - f[2] * u[1], f[2] * u[0] - f[0] * u[2], f[0] * u[1] - f[1] * u[0]};
      rx = dx * r[0] + dy * r[1] + dz * r[2];
      ry = dx * u[0] + dy * u[1] + dz * u[2];
      rz = -(dx * f[0] + dy * f[1] + dz * f[2]);
      // Doppler (FMOD: scale 1): f' = f·(c − vL·u)/(c − vS·u), u from the source to the listener
      float d = std::sqrt(dx * dx + dy * dy + dz * dz);
      if (d > 1e-3f) {
        float ux = -dx / d, uy = -dy / d, uz = -dz / d;
        float vl = s.lvel[0] * ux + s.lvel[1] * uy + s.lvel[2] * uz;
        float vs = v.vel[0] * ux + v.vel[1] * uy + v.vel[2] * uz;
        doppler = std::clamp((kSpeedOfSound - vl) / std::max(1.0f, kSpeedOfSound - vs), 0.5f, 2.0f);
      }
    }
    float d = std::sqrt(rx * rx + ry * ry + rz * rz);
    v.rel[0] = rx, v.rel[1] = ry, v.rel[2] = rz, v.dist = d;
    // linear rolloff (§9.2)
    float g = 1;
    if (d >= v.maxD) g = 0;
    else if (d > v.minD) g = (v.maxD - d) / std::max(1e-3f, v.maxD - v.minD);
    g *= vol;
    v.audibility = g;
    if (s.layout == Layout::Headphones && d > 1e-3f) {
      // data-free binaural (§16.3): interaural time difference (Woodworth, head radius 8.75 cm, ≤ 0.66 ms) on the far
      // ear, a level difference, a head-shadow low-pass on the far ear and a slight low-pass for sources behind
      v.binaural = true;
      float p = std::clamp(rx / d, -1.0f, 1.0f), lat = std::asin(p);
      float itd = 0.0875f / 343.0f * (std::fabs(lat) + std::sin(std::fabs(lat))) * float(s.rate);
      int farEar = p > 0 ? 0 : 1;  // left ear is far for a source on the right
      v.itdTarget[farEar] = itd;
      v.itdTarget[1 - farEar] = 0;
      float a = std::fabs(p);
      float gl = std::sqrt(0.5f * (1 - 0.6f * p)), gr = std::sqrt(0.5f * (1 + 0.6f * p));
      float behind = std::max(0.0f, rz / d);
      auto coeff = [&](float fc) { return 1.0f - std::exp(-2 * kPi * std::min(fc, 0.45f * float(s.rate)) / float(s.rate)); };
      float rear = 18000.0f * std::pow(0.35f, behind);
      float shadow = 18000.0f * std::pow(0.1f, a);
      v.lpTarget[farEar] = coeff(std::min(rear, shadow));
      v.lpTarget[1 - farEar] = coeff(rear);
      v.target[0][0] = g * gl;
      v.target[0][1] = g * gr;
    } else {
      float pan[kMaxCh];
      panGains(s, rx, ry, rz, d, pan);
      for (uint32_t c = 0; c < s.busCh; ++c) v.target[0][c] = g * pan[c];  // a stereo source is downmixed first
    }
  }
  v.rate = float(ci.sampleRate) / float(s.rate) * std::max(0.01f, v.pitch) * doppler;
}

// ---- reading source frames ----

// a stream frame (logical index i) into out[2]; false past the end of a non-looping stream
bool streamFrame(Voice& v, uint64_t i, float out[2]) {
  const uint32_t ch = v.clip->info.channels ? v.clip->info.channels : 1;
  constexpr uint32_t kChunk = 4096;
  int guard = 0;
  while (i >= v.bufStart + v.bufLen) {
    if (v.decEof) {
      if (!v.loop || ++guard > 2) return false;
      ma_decoder_seek_to_pcm_frame(&v.dec, 0);
      v.decEof = false;
    }
    uint32_t keep = std::min<uint32_t>(v.bufLen, 1);
    if (keep) std::memmove(v.buf.data(), v.buf.data() + size_t(v.bufLen - keep) * ch, sizeof(float) * ch * keep);
    v.bufStart += v.bufLen - keep;
    v.bufLen = keep;
    v.buf.resize(size_t(kChunk + 1) * ch);
    ma_uint64 got = 0;
    ma_result r = ma_decoder_read_pcm_frames(&v.dec, v.buf.data() + size_t(keep) * ch, kChunk, &got);
    v.bufLen += uint32_t(got);
    if (r != MA_SUCCESS || got < kChunk) v.decEof = true;
  }
  if (i < v.bufStart) return false;
  const float* p = v.buf.data() + size_t(i - v.bufStart) * ch;
  out[0] = p[0];
  out[1] = ch > 1 ? p[1] : p[0];
  return true;
}

bool pcmFrame(const Voice& v, uint64_t i, float out[2]) {
  const ClipInfo& ci = v.clip->info;
  if (ci.frames == 0) return false;
  if (i >= ci.frames) {
    if (!v.loop) return false;
    i %= ci.frames;
  }
  const uint32_t ch = ci.channels ? ci.channels : 1;
  const float* p = v.clip->pcm.data() + size_t(i) * ch;
  out[0] = p[0];
  out[1] = ch > 1 ? p[1] : p[0];
  return true;
}

// Audio thread: adds one voice into the bus.
void mixVoice(State& s, Voice& v, float* bus, uint32_t frames) {
  const ClipInfo& ci = v.clip->info;
  const bool stream = ci.stream;
  const uint32_t bc = s.busCh;
  if (!v.started) {
    std::memcpy(v.cur, v.target, sizeof v.cur);
    std::memcpy(v.itd, v.itdTarget, sizeof v.itd);
    std::memcpy(v.lp, v.lpTarget, sizeof v.lp);
    v.started = true;
  }
  if (v.virt && !stream) {  // virtual: advance only
    v.frame += double(v.rate) * frames;
    if (ci.frames && v.frame >= double(ci.frames)) {
      if (v.loop) v.frame = std::fmod(v.frame, double(ci.frames));
      else v.finished = true;
    }
    std::memcpy(v.cur, v.target, sizeof v.cur);
    return;
  }
  float dg[2][kMaxCh];
  const float inv = 1.0f / float(frames);
  for (int r = 0; r < 2; ++r)
    for (uint32_t c = 0; c < bc; ++c) dg[r][c] = (v.target[r][c] - v.cur[r][c]) * inv;
  float ditd[2] = {(v.itdTarget[0] - v.itd[0]) * inv, (v.itdTarget[1] - v.itd[1]) * inv};
  float dlp[2] = {(v.lpTarget[0] - v.lp[0]) * inv, (v.lpTarget[1] - v.lp[1]) * inv};
  const bool mute = v.virt;
  for (uint32_t n = 0; n < frames; ++n) {
    uint64_t i = uint64_t(v.frame);
    float frac = float(v.frame - double(i));
    float a[2], b[2];
    bool okA = stream ? streamFrame(v, i, a) : pcmFrame(v, i, a);
    if (!okA) {
      v.finished = true;
      break;
    }
    bool okB = stream ? streamFrame(v, i + 1, b) : pcmFrame(v, i + 1, b);
    if (!okB) b[0] = a[0], b[1] = a[1];
    float s0 = a[0] + (b[0] - a[0]) * frac, s1 = a[1] + (b[1] - a[1]) * frac;
    for (int r = 0; r < 2; ++r)
      for (uint32_t c = 0; c < bc; ++c) v.cur[r][c] += dg[r][c];
    if (!mute) {
      float* out = bus + size_t(n) * bc;
      if (v.binaural) {
        float m = ci.channels >= 2 ? 0.5f * (s0 + s1) : s0;
        v.hist[v.histPos] = m;
        for (int e = 0; e < 2; ++e) {
          v.itd[e] += ditd[e];
          v.lp[e] += dlp[e];
          float dly = std::clamp(v.itd[e], 0.0f, float(kHist - 2));
          int di = int(dly);
          float df = dly - float(di);
          float x0 = v.hist[(v.histPos - di + kHist) % kHist], x1 = v.hist[(v.histPos - di - 1 + kHist) % kHist];
          float x = x0 + (x1 - x0) * df;
          v.lpState[e] += v.lp[e] * (x - v.lpState[e]);
          out[e] += v.lpState[e] * v.cur[0][e];
        }
        v.histPos = (v.histPos + 1) % kHist;
      } else if (v.stereoRows) {
        for (uint32_t c = 0; c < bc; ++c) out[c] += s0 * v.cur[0][c] + s1 * v.cur[1][c];
      } else {
        float m = ci.channels >= 2 ? 0.5f * (s0 + s1) : s0;
        for (uint32_t c = 0; c < bc; ++c) out[c] += m * v.cur[0][c];
      }
    }
    v.frame += v.rate;
    if (!stream && ci.frames && v.frame >= double(ci.frames)) {
      if (v.loop) v.frame -= double(ci.frames) * std::floor(v.frame / double(ci.frames));
      else if (uint64_t(v.frame) >= ci.frames) {
        v.finished = true;
        break;
      }
    }
  }
  std::memcpy(v.cur, v.target, sizeof v.cur);
  std::memcpy(v.itd, v.itdTarget, sizeof v.itd);
  std::memcpy(v.lp, v.lpTarget, sizeof v.lp);
}

void dataCallback(ma_device* device, void* output, const void*, ma_uint32 frames) {
  State& s = *static_cast<State*>(device->pUserData);
  float* out = static_cast<float*>(output);
  const uint32_t bc = s.busCh, oc = s.outCh;
  {
    std::lock_guard<std::mutex> g(s.lock);
    s.bus.assign(size_t(frames) * bc, 0.0f);
    for (auto& [id, vp] : s.voices) {
      Voice& v = *vp;
      if (v.stopped || v.finished || v.paused) continue;
      mixVoice(s, v, s.bus.data(), frames);
    }
    const float m = std::clamp(s.master, 0.0f, 1.0f);
    for (uint32_t n = 0; n < frames; ++n) {
      const float* b = s.bus.data() + size_t(n) * bc;
      float* o = out + size_t(n) * oc;
      if (s.layout == Layout::Prologic) {
        // passive Lt/Rt matrix of the 5.0 bus (FL FR FC BL BR): centre −3 dB to both, the surrounds summed −3 dB and
        // put in antiphase (the ±90° phase shift of a real encoder is left out)
        float sur = 0.70710678f * (b[3] + b[4]);
        o[0] = m * (b[0] + 0.70710678f * b[2] - 0.70710678f * sur);
        o[1] = m * (b[1] + 0.70710678f * b[2] + 0.70710678f * sur);
      } else {
        for (uint32_t c = 0; c < oc; ++c) o[c] = m * b[c];
      }
      for (uint32_t c = 0; c < oc; ++c) {
        o[c] = std::clamp(o[c], -1.0f, 1.0f);
        if (c < kMaxCh) s.sumSq[c] += double(o[c]) * o[c];
      }
    }
    s.statFrames += frames;
  }
  if (s.capture) {
    std::lock_guard<std::mutex> g(s.captureLock);
    size_t at = s.captureQueue.size();
    s.captureQueue.resize(at + size_t(frames) * oc);
    for (size_t k = 0; k < size_t(frames) * oc; ++k) s.captureQueue[at + k] = int16_t(std::lrint(out[k] * 32767.0f));
  }
}

void writeWavHeader(FILE* f, uint32_t channels, uint32_t rate, uint64_t dataBytes) {
  auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
  uint32_t data = uint32_t(std::min<uint64_t>(dataBytes, 0xFFFFFFF0u - 36));
  std::fwrite("RIFF", 1, 4, f);
  u32(36 + data);
  std::fwrite("WAVEfmt ", 1, 8, f);
  u32(16);
  u16(1);  // PCM
  u16(uint16_t(channels));
  u32(rate);
  u32(rate * channels * 2);
  u16(uint16_t(channels * 2));
  u16(16);
  std::fwrite("data", 1, 4, f);
  u32(data);
}

void flushCapture(State& s) {
  if (!s.capture) return;
  std::vector<int16_t> q;
  {
    std::lock_guard<std::mutex> g(s.captureLock);
    q.swap(s.captureQueue);
  }
  if (q.empty()) return;
  std::fwrite(q.data(), sizeof(int16_t), q.size(), s.capture);
  s.captureBytes += q.size() * sizeof(int16_t);
}

Voice* voiceOf(int id) {
  auto it = st().voices.find(id);
  return it == st().voices.end() ? nullptr : it->second.get();
}

}  // namespace

Layout layoutFromName(const std::string& name) {
  for (int i = 0; i < int(Layout::Count); ++i)
    if (name == kLayoutNames[i]) return Layout(i);
  return Layout::Default;
}

const char* layoutName(Layout l) { return int(l) >= 0 && l < Layout::Count ? kLayoutNames[int(l)] : "Default"; }

std::vector<std::string> deviceNames() {
  std::vector<std::string> out;
  ma_context ctx;
  if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) return out;
  ma_device_info* infos = nullptr;
  ma_uint32 n = 0;
  if (ma_context_get_devices(&ctx, &infos, &n, nullptr, nullptr) == MA_SUCCESS)
    for (ma_uint32 i = 0; i < n; ++i) out.emplace_back(infos[i].name);
  ma_context_uninit(&ctx);
  return out;
}

bool init(const InitOptions& o) {
  State& s = st();
  if (s.open) return true;
  if (ma_context_init(nullptr, 0, nullptr, &s.context) != MA_SUCCESS) {
    LOG_WARN("audio: no audio backend, the game runs silent");
    return false;
  }
  s.hasContext = true;
  ma_device_id chosen{};
  bool haveId = false;
  if (o.device != "Default") {
    ma_device_info* infos = nullptr;
    ma_uint32 n = 0;
    if (ma_context_get_devices(&s.context, &infos, &n, nullptr, nullptr) == MA_SUCCESS)
      for (ma_uint32 i = 0; i < n; ++i)
        if (o.device == infos[i].name) chosen = infos[i].id, haveId = true;
    if (!haveId) LOG_WARN("audio: device \"%s\" not found, using the default", o.device.c_str());
  }
  s.layout = o.layout;
  ma_device_config dc = ma_device_config_init(ma_device_type_playback);
  dc.playback.format = ma_format_f32;
  dc.playback.pDeviceID = haveId ? &chosen : nullptr;
  dc.sampleRate = 0;  // the device's rate
  dc.dataCallback = dataCallback;
  dc.pUserData = &s;
  ma_channel map[kMaxCh] = {};
  uint32_t ch = 0;
  switch (s.layout) {
    case Layout::Mono: ch = 1, map[0] = MA_CHANNEL_MONO; break;
    case Layout::Stereo: case Layout::Prologic: case Layout::Headphones:
      ch = 2, map[0] = MA_CHANNEL_FRONT_LEFT, map[1] = MA_CHANNEL_FRONT_RIGHT; break;
    case Layout::Quad:
      ch = 4, map[0] = MA_CHANNEL_FRONT_LEFT, map[1] = MA_CHANNEL_FRONT_RIGHT, map[2] = MA_CHANNEL_BACK_LEFT, map[3] = MA_CHANNEL_BACK_RIGHT;
      break;
    case Layout::Surround:
      ch = 5, map[0] = MA_CHANNEL_FRONT_LEFT, map[1] = MA_CHANNEL_FRONT_RIGHT, map[2] = MA_CHANNEL_FRONT_CENTER,
      map[3] = MA_CHANNEL_BACK_LEFT, map[4] = MA_CHANNEL_BACK_RIGHT;
      break;
    case Layout::P51:
      ch = 6, map[0] = MA_CHANNEL_FRONT_LEFT, map[1] = MA_CHANNEL_FRONT_RIGHT, map[2] = MA_CHANNEL_FRONT_CENTER,
      map[3] = MA_CHANNEL_LFE, map[4] = MA_CHANNEL_SIDE_LEFT, map[5] = MA_CHANNEL_SIDE_RIGHT;
      break;
    case Layout::P71:
      ch = 8, map[0] = MA_CHANNEL_FRONT_LEFT, map[1] = MA_CHANNEL_FRONT_RIGHT, map[2] = MA_CHANNEL_FRONT_CENTER,
      map[3] = MA_CHANNEL_LFE, map[4] = MA_CHANNEL_BACK_LEFT, map[5] = MA_CHANNEL_BACK_RIGHT, map[6] = MA_CHANNEL_SIDE_LEFT,
      map[7] = MA_CHANNEL_SIDE_RIGHT;
      break;
    default: ch = 0; break;  // the device's own layout
  }
  dc.playback.channels = ch;
  dc.playback.pChannelMap = ch ? map : nullptr;
  if (ma_device_init(&s.context, &dc, &s.device) != MA_SUCCESS) {
    LOG_WARN("audio: no output device, the game runs silent");
    ma_context_uninit(&s.context);
    s.hasContext = false;
    return false;
  }
  s.outCh = std::min<uint32_t>(s.device.playback.channels, kMaxCh);
  s.rate = s.device.sampleRate;
  if (s.layout == Layout::Prologic) {
    s.busCh = 5;
    s.map[0] = MA_CHANNEL_FRONT_LEFT, s.map[1] = MA_CHANNEL_FRONT_RIGHT, s.map[2] = MA_CHANNEL_FRONT_CENTER;
    s.map[3] = MA_CHANNEL_BACK_LEFT, s.map[4] = MA_CHANNEL_BACK_RIGHT;
  } else {
    s.busCh = s.outCh;
    for (uint32_t c = 0; c < s.outCh; ++c) s.map[c] = s.device.playback.channelMap[c];
    if (s.outCh == 1) s.map[0] = MA_CHANNEL_MONO;
  }
  buildSpeakers(s);
  if (!o.capture.empty()) {
    s.capture = std::fopen(o.capture.c_str(), "wb");
    if (s.capture) writeWavHeader(s.capture, s.outCh, s.rate, 0);
    else LOG_WARN("audio: cannot write the capture file %s", o.capture.c_str());
  }
  s.open = true;
  if (ma_device_start(&s.device) != MA_SUCCESS) LOG_WARN("audio: the device did not start");
  LOG_INFO("audio: %s", describeOutput().c_str());
  return true;
}

void shutdown() {
  State& s = st();
  if (s.open) {
    ma_device_uninit(&s.device);  // stops the callback
    s.open = false;
  }
  if (s.hasContext) ma_context_uninit(&s.context), s.hasContext = false;
  {
    std::lock_guard<std::mutex> g(s.lock);
    s.voices.clear();
  }
  if (s.capture) {
    flushCapture(s);
    std::fseek(s.capture, 0, SEEK_SET);
    writeWavHeader(s.capture, s.outCh, s.rate, s.captureBytes);
    std::fclose(s.capture);
    s.capture = nullptr;
    LOG_INFO("audio: capture closed (%.1f s)", double(s.captureBytes) / 2.0 / std::max(1u, s.outCh) / std::max(1u, s.rate));
  }
}

bool available() { return st().open; }
uint32_t outputChannels() { return st().outCh; }
uint32_t sampleRate() { return st().rate; }
Layout layout() { return st().layout; }

std::string describeOutput() {
  State& s = st();
  std::string m;
  for (uint32_t c = 0; c < s.outCh; ++c) m += std::string(c ? " " : "") + channelName(s.layout == Layout::Prologic ? ma_channel(c ? MA_CHANNEL_FRONT_RIGHT : MA_CHANNEL_FRONT_LEFT) : s.map[c]);
  char b[256];
  std::snprintf(b, sizeof b, "%u Hz, %u channels (%s), layout %s%s, device \"%s\"", s.rate, s.outCh, m.c_str(), layoutName(s.layout),
                s.layout == Layout::Prologic ? " (5.0 bus, Lt/Rt encoded)" : "", s.open ? s.device.playback.name : "");
  return b;
}

ClipRef loadClip(std::vector<uint8_t> bytes, bool stream, std::string* error) {
  ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 0, 0);  // native channels and rate
  ma_decoder dec;
  if (ma_decoder_init_memory(bytes.data(), bytes.size(), &dc, &dec) != MA_SUCCESS) {
    if (error) *error = "unsupported or damaged audio file";
    return nullptr;
  }
  auto clip = std::make_shared<Clip>();
  ma_format fmt;
  ma_uint32 ch = 0, sr = 0;
  ma_decoder_get_data_format(&dec, &fmt, &ch, &sr, nullptr, 0);
  clip->info.channels = ch;
  clip->info.sampleRate = sr;
  clip->info.stream = stream;
  ma_uint64 len = 0;
  if (ma_decoder_get_length_in_pcm_frames(&dec, &len) == MA_SUCCESS) clip->info.frames = len;
  if (!stream) {
    std::vector<float>& pcm = clip->pcm;
    pcm.reserve(size_t(len) * ch);
    float chunk[4096];
    const ma_uint64 per = sizeof chunk / sizeof(float) / (ch ? ch : 1);
    for (;;) {
      ma_uint64 got = 0;
      ma_result r = ma_decoder_read_pcm_frames(&dec, chunk, per, &got);
      pcm.insert(pcm.end(), chunk, chunk + size_t(got) * ch);
      if (r != MA_SUCCESS || got < per) break;
    }
    clip->info.frames = ch ? pcm.size() / ch : 0;
  }
  ma_decoder_uninit(&dec);
  if (ch == 0 || sr == 0) {
    if (error) *error = "no audio data";
    return nullptr;
  }
  if (stream) clip->bytes = std::move(bytes);
  return clip;
}

ClipInfo clipInfo(const ClipRef& clip) { return clip ? clip->info : ClipInfo{}; }

int play(const ClipRef& clip, const VoiceParams& p) {
  State& s = st();
  if (!s.open || !clip) return 0;
  auto v = std::make_unique<Voice>();
  v->clip = clip;
  if (clip->info.stream) {
    // decoder set up (and seeked) here, outside the lock
    ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 0, 0);
    if (ma_decoder_init_memory(clip->bytes.data(), clip->bytes.size(), &dc, &v->dec) != MA_SUCCESS) return 0;
    v->hasDec = true;
    if (p.startSeconds > 0) {
      uint64_t f = uint64_t(p.startSeconds * clip->info.sampleRate);
      if (clip->info.frames) f = std::min(f, clip->info.frames);
      ma_decoder_seek_to_pcm_frame(&v->dec, f);
      v->bufStart = f;
      v->frame = double(f);
    }
  } else {
    if (clip->pcm.empty()) return 0;
    v->frame = std::min(p.startSeconds * clip->info.sampleRate, double(clip->info.frames));
  }
  v->volume = p.volume;
  v->pitch = p.pitch;
  v->loop = p.loop;
  v->paused = p.paused;
  v->spatial = p.spatial;
  v->relative = p.relative;
  std::memcpy(v->pos, p.pos, sizeof v->pos);
  std::memcpy(v->vel, p.vel, sizeof v->vel);
  v->minD = p.minDistance;
  v->maxD = p.maxDistance;
  std::lock_guard<std::mutex> g(s.lock);
  computeVoice(s, *v);
  int id = s.nextId++;
  v->id = id;
  s.voices[id] = std::move(v);
  return id;
}

bool isPlaying(int id) {
  std::lock_guard<std::mutex> g(st().lock);
  Voice* v = voiceOf(id);
  return v && !v->stopped && !v->finished;
}

void stop(int id) {
  std::lock_guard<std::mutex> g(st().lock);
  if (Voice* v = voiceOf(id)) v->stopped = true;
}

void setPaused(int id, bool paused) {
  std::lock_guard<std::mutex> g(st().lock);
  if (Voice* v = voiceOf(id)) v->paused = paused;
}

void setVolume(int id, float volume) {
  std::lock_guard<std::mutex> g(st().lock);
  if (Voice* v = voiceOf(id)) v->volume = volume, computeVoice(st(), *v);
}

void setPitch(int id, float pitch) {
  std::lock_guard<std::mutex> g(st().lock);
  if (Voice* v = voiceOf(id)) v->pitch = pitch, computeVoice(st(), *v);
}

void setLooping(int id, bool loop) {
  std::lock_guard<std::mutex> g(st().lock);
  if (Voice* v = voiceOf(id)) v->loop = loop;
}

void set3D(int id, bool relative, const float pos[3], const float vel[3], float minDistance, float maxDistance) {
  std::lock_guard<std::mutex> g(st().lock);
  if (Voice* v = voiceOf(id)) {
    v->relative = relative;
    std::memcpy(v->pos, pos, sizeof v->pos);
    std::memcpy(v->vel, vel, sizeof v->vel);
    v->minD = minDistance;
    v->maxD = maxDistance;
  }
}

double positionSeconds(int id) {
  std::lock_guard<std::mutex> g(st().lock);
  Voice* v = voiceOf(id);
  if (!v || !v->clip->info.sampleRate) return 0;
  double f = v->frame;
  if (v->clip->info.frames) f = std::fmod(f, double(v->clip->info.frames));
  return f / v->clip->info.sampleRate;
}

void setListener(const float pos[3], const float forward[3], const float up[3], const float vel[3]) {
  State& s = st();
  std::lock_guard<std::mutex> g(s.lock);
  std::memcpy(s.lpos, pos, sizeof s.lpos);
  float l = std::sqrt(forward[0] * forward[0] + forward[1] * forward[1] + forward[2] * forward[2]);
  if (l > 1e-6f)
    for (int k = 0; k < 3; ++k) s.lfwd[k] = forward[k] / l;
  std::memcpy(s.lup, up, sizeof s.lup);
  std::memcpy(s.lvel, vel, sizeof s.lvel);
}

void setMasterVolume(float volume) {
  std::lock_guard<std::mutex> g(st().lock);
  st().master = volume;
}

void update() {
  State& s = st();
  if (s.open) {
    std::vector<std::unique_ptr<Voice>> dead;  // freed outside the lock
    {
      std::lock_guard<std::mutex> g(s.lock);
      std::vector<Voice*> live;
      s.pausedCount = 0;
      for (auto it = s.voices.begin(); it != s.voices.end();) {
        Voice& v = *it->second;
        if (v.stopped || v.finished) {
          dead.push_back(std::move(it->second));
          it = s.voices.erase(it);
          continue;
        }
        computeVoice(s, v);
        if (v.paused) ++s.pausedCount;
        else live.push_back(&v);
        ++it;
      }
      std::sort(live.begin(), live.end(), [](const Voice* a, const Voice* b) { return a->audibility > b->audibility; });
      s.mixedCount = s.virtualCount = 0;
      for (size_t k = 0; k < live.size(); ++k) {
        // beyond the cap, and silent ones (out of range) are virtual
        live[k]->virt = int(k) >= kMaxMixed || live[k]->audibility <= 0;
        ++(live[k]->virt ? s.virtualCount : s.mixedCount);
      }
    }
  }
  flushCapture(s);
}

Stats takeStats() {
  State& s = st();
  Stats out;
  std::lock_guard<std::mutex> g(s.lock);
  out.voices = int(s.voices.size());
  out.mixed = s.mixedCount;
  out.virtualized = s.virtualCount;
  out.paused = s.pausedCount;
  out.channels = s.outCh;
  for (uint32_t c = 0; c < s.outCh && c < kMaxCh; ++c) {
    out.rms[c] = s.statFrames ? float(std::sqrt(s.sumSq[c] / double(s.statFrames))) : 0.0f;
    s.sumSq[c] = 0;
  }
  s.statFrames = 0;
  std::vector<const Voice*> sp;
  for (auto& [id, v] : s.voices)
    if (v->spatial && !v->paused && !v->stopped && !v->finished) sp.push_back(v.get());
  std::sort(sp.begin(), sp.end(), [](const Voice* a, const Voice* b) { return a->audibility > b->audibility; });
  for (size_t k = 0; k < sp.size() && k < 4; ++k) {
    Stats::Top t;
    t.voice = sp[k]->id;
    t.gain = sp[k]->audibility;
    t.x = sp[k]->rel[0], t.z = sp[k]->rel[2], t.d = sp[k]->dist;
    for (uint32_t c = 0; c < s.busCh && c < 8; ++c) t.ch[c] = sp[k]->target[0][c];
    t.rate = sp[k]->rate;
    out.top.push_back(t);
  }
  return out;
}

}  // namespace regs::audio
