#pragma once
// The port's audio output (replaces FMOD Ex, which the original's GSystem.dll uses; docs/audio/README.md). A thin layer
// over miniaudio (third_party/miniaudio, public domain / MIT-0): clips are decoded from the game's archives (WAV
// effects decoded once to PCM, MP3 music streamed from the compressed bytes in memory), every play is a voice of its
// own (FMOD's playSound makes a new channel each time), voices belong to a volume group, 3D voices are panned and
// attenuated around the listener. Headless runs (tools, tests) open no device: loading still decodes, playing is a
// no-op.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace regs::audio {

enum class Group : int { Effects = 0, Music = 1, Count };

struct Clip;  // decoded PCM, or the encoded bytes of a stream
using ClipRef = std::shared_ptr<Clip>;

struct ClipInfo {
  uint32_t channels = 0, sampleRate = 0;
  uint64_t frames = 0;  // 0 when unknown (some streams)
  bool stream = false;
};

// Open the output device (false when there is none; the game then runs silent). Headless: no device.
bool init(bool headless);
void shutdown();
bool available();  // a device is open

// Decode a WAV/MP3/FLAC file's bytes. stream: keep the encoded bytes and decode while playing (music).
ClipRef loadClip(std::vector<uint8_t> bytes, bool stream, std::string* error = nullptr);
ClipInfo clipInfo(const ClipRef& clip);

struct VoiceParams {
  Group group = Group::Effects;
  float volume = 1.0f;
  float pitch = 1.0f;
  bool loop = false;
  bool paused = false;         // start paused (FMOD playSound(paused = true))
  bool positional = false;     // 3D: panned and attenuated around the listener
  float x = 0, y = 0, z = 0;   // world position (3D voices)
  float minDistance = 1.0f, maxDistance = 10000.0f;
};

// Start a voice; returns its id (> 0), or 0 when nothing plays (no device, no clip).
int play(const ClipRef& clip, const VoiceParams& params);
bool isPlaying(int voice);     // started and not yet finished or stopped (paused counts as playing)
void stop(int voice);
void setPaused(int voice, bool paused);
void setVolume(int voice, float volume);
void setPitch(int voice, float pitch);
void setLooping(int voice, bool loop);
void setPosition(int voice, float x, float y, float z);
void setDistances(int voice, float minDistance, float maxDistance);

// The listener (the 3D camera): position, look direction and up vector in world space.
void setListener(float x, float y, float z, float fx, float fy, float fz, float ux, float uy, float uz);
void setGroupVolume(Group group, float volume);
void setMasterVolume(float volume);

// Once per frame: frees finished voices.
void update();
size_t liveVoices();

}  // namespace regs::audio
