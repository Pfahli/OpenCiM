#include "audio/audio.h"

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

struct Voice {
  ClipRef clip;
  ma_audio_buffer_ref buffer{};  // PCM clips
  ma_decoder decoder{};          // streams
  bool hasBuffer = false, hasDecoder = false;
  ma_sound sound{};
  bool soundInit = false;
  bool stopped = false;  // stop() was called (paused voices keep playing state)
  bool paused = false;
  bool loop = false;
  ~Voice() {
    if (soundInit) ma_sound_uninit(&sound);
    if (hasDecoder) ma_decoder_uninit(&decoder);
    if (hasBuffer) ma_audio_buffer_ref_uninit(&buffer);
  }
};

struct State {
  bool open = false;
  ma_engine engine{};
  ma_sound_group groups[int(Group::Count)]{};
  std::map<int, std::unique_ptr<Voice>> voices;
  int nextId = 1;
};

State& st() {
  static State s;
  return s;
}

Voice* voiceOf(int id) {
  auto it = st().voices.find(id);
  return it == st().voices.end() ? nullptr : it->second.get();
}

}  // namespace

bool init(bool headless) {
  State& s = st();
  if (s.open || headless) return s.open;
  ma_engine_config cfg = ma_engine_config_init();
  if (ma_engine_init(&cfg, &s.engine) != MA_SUCCESS) {
    LOG_WARN("audio: no output device, the game runs silent");
    return false;
  }
  for (auto& g : s.groups) ma_sound_group_init(&s.engine, 0, nullptr, &g);
  s.open = true;
  LOG_INFO("audio: %u Hz, %u channels", ma_engine_get_sample_rate(&s.engine), ma_engine_get_channels(&s.engine));
  return true;
}

void shutdown() {
  State& s = st();
  s.voices.clear();
  if (!s.open) return;
  for (auto& g : s.groups) ma_sound_group_uninit(&g);
  ma_engine_uninit(&s.engine);
  s.open = false;
}

bool available() { return st().open; }

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
  if (stream) clip->bytes = std::move(bytes);
  return clip;
}

ClipInfo clipInfo(const ClipRef& clip) { return clip ? clip->info : ClipInfo{}; }

int play(const ClipRef& clip, const VoiceParams& p) {
  State& s = st();
  if (!s.open || !clip) return 0;
  auto v = std::make_unique<Voice>();
  v->clip = clip;
  ma_data_source* src = nullptr;
  if (clip->info.stream) {
    ma_decoder_config dc = ma_decoder_config_init(ma_format_f32, 0, 0);
    if (ma_decoder_init_memory(clip->bytes.data(), clip->bytes.size(), &dc, &v->decoder) != MA_SUCCESS) return 0;
    v->hasDecoder = true;
    src = &v->decoder;
  } else {
    if (clip->pcm.empty()) return 0;
    if (ma_audio_buffer_ref_init(ma_format_f32, clip->info.channels, clip->pcm.data(), clip->info.frames, &v->buffer) !=
        MA_SUCCESS)
      return 0;
    v->buffer.sampleRate = clip->info.sampleRate;
    v->hasBuffer = true;
    src = &v->buffer;
  }
  ma_uint32 flags = p.positional ? 0 : MA_SOUND_FLAG_NO_SPATIALIZATION;
  if (ma_sound_init_from_data_source(&s.engine, src, flags, &s.groups[int(p.group)], &v->sound) != MA_SUCCESS) return 0;
  v->soundInit = true;
  ma_sound_set_volume(&v->sound, p.volume);
  ma_sound_set_pitch(&v->sound, p.pitch);
  ma_sound_set_looping(&v->sound, p.loop ? MA_TRUE : MA_FALSE);
  v->loop = p.loop;
  if (p.positional) {
    ma_sound_set_position(&v->sound, p.x, p.y, p.z);
    ma_sound_set_min_distance(&v->sound, p.minDistance);
    ma_sound_set_max_distance(&v->sound, p.maxDistance);
  }
  v->paused = p.paused;
  if (!p.paused) ma_sound_start(&v->sound);
  int id = s.nextId++;
  s.voices[id] = std::move(v);
  return id;
}

bool isPlaying(int id) {
  Voice* v = voiceOf(id);
  return v && !v->stopped && (v->paused || v->loop || !ma_sound_at_end(&v->sound));
}

void stop(int id) {
  if (Voice* v = voiceOf(id)) {
    ma_sound_stop(&v->sound);
    v->stopped = true;
  }
}

void setPaused(int id, bool paused) {
  Voice* v = voiceOf(id);
  if (!v || v->stopped || v->paused == paused) return;
  v->paused = paused;
  if (paused) ma_sound_stop(&v->sound);
  else ma_sound_start(&v->sound);
}

void setVolume(int id, float volume) {
  if (Voice* v = voiceOf(id)) ma_sound_set_volume(&v->sound, volume);
}
void setPitch(int id, float pitch) {
  if (Voice* v = voiceOf(id)) ma_sound_set_pitch(&v->sound, pitch);
}
void setLooping(int id, bool loop) {
  if (Voice* v = voiceOf(id)) ma_sound_set_looping(&v->sound, loop ? MA_TRUE : MA_FALSE), v->loop = loop;
}
void setPosition(int id, float x, float y, float z) {
  if (Voice* v = voiceOf(id)) ma_sound_set_position(&v->sound, x, y, z);
}
void setDistances(int id, float minDistance, float maxDistance) {
  if (Voice* v = voiceOf(id)) {
    ma_sound_set_min_distance(&v->sound, minDistance);
    ma_sound_set_max_distance(&v->sound, maxDistance);
  }
}

void setListener(float x, float y, float z, float fx, float fy, float fz, float ux, float uy, float uz) {
  State& s = st();
  if (!s.open) return;
  ma_engine_listener_set_position(&s.engine, 0, x, y, z);
  ma_engine_listener_set_direction(&s.engine, 0, fx, fy, fz);
  ma_engine_listener_set_world_up(&s.engine, 0, ux, uy, uz);
}

void setGroupVolume(Group group, float volume) {
  State& s = st();
  if (s.open) ma_sound_group_set_volume(&s.groups[int(group)], volume);
}

void setMasterVolume(float volume) {
  State& s = st();
  if (s.open) ma_engine_set_volume(&s.engine, volume);
}

void update() {
  State& s = st();
  for (auto it = s.voices.begin(); it != s.voices.end();) {
    Voice& v = *it->second;
    if (v.stopped || (!v.paused && !v.loop && ma_sound_at_end(&v.sound))) it = s.voices.erase(it);
    else ++it;
  }
}

size_t liveVoices() { return st().voices.size(); }

}  // namespace regs::audio
