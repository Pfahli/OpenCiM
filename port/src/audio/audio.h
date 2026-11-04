#pragma once
// The port's audio output (replaces FMOD Ex, which the original's GSystem.dll uses; docs/audio/README.md §16). A small
// software mixer of its own on a miniaudio playback device (third_party/miniaudio, public domain / MIT-0; only its
// device layer and decoders are used): clips are decoded from the game's archives (WAV effects decoded once to PCM,
// MP3 music and ambience decoded while playing from the compressed bytes in memory, like FMOD's compressed samples),
// voices are resampled (linear, like FMOD Ex's default resampler) at file rate · pitch · Doppler, 3D voices get FMOD's
// linear rolloff and are panned over the chosen speaker layout, 2D voices pass through. Every voice belongs to a volume
// group (the port's Effects / Ambience / Interface sliders, docs/audio/README.md §16.2); one master gain
// (set_audio_volume) sits after the mix, on top of the groups. Headless runs (tools, tests) open no device: loading still decodes, playing is a no-op.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace regs::audio {

// The $audiospeakers values (§4.1, audio_speakers()) plus the port's Headphones (§16.3).
enum class Layout : int { Default, Mono, Stereo, Quad, Surround, P51, P71, Prologic, Headphones, Count };
Layout layoutFromName(const std::string& name);  // unknown names → Default (as the original: no setSpeakerMode)
const char* layoutName(Layout l);                // "Default", "Mono", ..., "5p1", "7p1", "Prologic", "Headphones"

struct InitOptions {
  std::string device = "Default";  // a name from deviceNames(); "Default" (or an unknown name) = the system default
  Layout layout = Layout::Default;
  std::string capture;             // also write the final mix to this WAV file (16-bit PCM, REGS_AUDIO_CAPTURE)
};

// Open the output device (false when there is none; the game then runs silent).
bool init(const InitOptions& options);
void shutdown();
bool available();  // a device is open
uint32_t outputChannels();  // channels the device is fed (2 for Prologic and Headphones)
uint32_t sampleRate();
Layout layout();
std::string describeOutput();  // e.g. "48000 Hz, 6 channels (FL FR FC LFE SL SR), layout 5p1"

// The playback devices' names (a temporary context; works before init).
std::vector<std::string> deviceNames();

// Volume groups (§16.2, §18.2): Effects = the world (engine voices: vehicles, items, animals, bells, doors),
// Ambience = the area loops, Interface = UI and other script sounds, Music = the music tracks (gain 1: the original's
// $musicVolume is a per-object volume set by the scripts).
enum class Group : int { Effects, Ambience, Interface, Music, Count };
const char* groupName(Group g);  // "effects", "ambience", "interface", "music"

struct Clip;  // decoded PCM, or the encoded bytes of a stream
using ClipRef = std::shared_ptr<Clip>;

struct ClipInfo {
  uint32_t channels = 0, sampleRate = 0;
  uint64_t frames = 0;  // 0 when unknown
  bool stream = false;
  double seconds() const { return sampleRate ? double(frames) / sampleRate : 0.0; }
};

// Decode a WAV/MP3/FLAC file's bytes. stream: keep the encoded bytes and decode while playing (music).
ClipRef loadClip(std::vector<uint8_t> bytes, bool stream, std::string* error = nullptr);
ClipInfo clipInfo(const ClipRef& clip);

struct VoiceParams {
  float volume = 1.0f;        // clamped to 0..1 like an FMOD channel
  float pitch = 1.0f;         // playback rate factor (the file's rate is matched to the device's)
  bool loop = false;
  bool paused = false;
  bool spatial = false;       // 3D (a clip loaded 3D): attenuated and panned around the listener; else 2D pass-through
  bool relative = false;      // head-relative: pos and vel are in listener space (x right, y up, −z forward)
  float pos[3] = {0, 0, 0};   // world units
  float vel[3] = {0, 0, 0};   // units per second (Doppler)
  float minDistance = 10.0f, maxDistance = 100.0f;
  double startSeconds = 0;    // start offset (resume after pause)
  Group group = Group::Interface;
};

// Start a voice; returns its id (> 0), or 0 when nothing plays (no device, no clip).
int play(const ClipRef& clip, const VoiceParams& params);
bool isPlaying(int voice);  // started and not yet finished or stopped (paused counts as playing)
void stop(int voice);
void setPaused(int voice, bool paused);
void setVolume(int voice, float volume);
void setPitch(int voice, float pitch);
void setLooping(int voice, bool loop);
void set3D(int voice, bool relative, const float pos[3], const float vel[3], float minDistance, float maxDistance);
double positionSeconds(int voice);  // within the clip (wrapped for loops); 0 for an unknown voice

// The listener (the 3D camera eye): position, forward and up in world space, velocity for Doppler.
void setListener(const float pos[3], const float forward[3], const float up[3], const float vel[3]);
void setMasterVolume(float volume);
// A group's gain (0..1), applied with each voice's channel volume (so it also ranks the voices for the voice cap)
void setGroupVolume(Group g, float volume);
float groupVolume(Group g);

// Once per frame: 3D gains, Doppler and pan of every voice, the voice cap (64 mixed, the quietest beyond are
// virtual: they advance silently), finished voices freed, the capture file written.
void update();

struct Stats {
  int voices = 0, mixed = 0, virtualized = 0, paused = 0;
  float rms[8] = {};  // per output channel since the last call, after the master gain
  uint32_t channels = 0;
  float groupRms[int(Group::Count)] = {};  // per group: RMS over the bus channels, after group and master gain
  struct Top {
    int voice = 0;
    Group group = Group::Effects;
    float gain = 0;               // distance gain · volume
    float x = 0, z = 0, d = 0;    // listener space (x right, −z forward) and distance; 0 for 2D voices
    float ch[8] = {};             // gain per bus channel
    float rate = 1;               // resampling ratio incl. pitch and Doppler
  };
  std::vector<Top> top;  // the loudest 3D voices (up to 4)
};
Stats takeStats();  // and resets the RMS window

}  // namespace regs::audio
