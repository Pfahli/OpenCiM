#pragma once
// The script side of sound (docs/audio/README.md): enable_sounds / set_audio_volume / audio_devices / audio_speakers,
// gsSound and gsMusic objects with the path-keyed sound cache, gsListener and the per-frame listener update, and the
// engine-side world voices the simulation plays from template sounds (engine/sim_sounds.cpp). Output: audio/audio.h.
#include <string>

#include "audio/audio.h"
#include "render/math3d.h"
#include "script/vm.h"

namespace regs::engine {

void registerSoundNatives(script::VM& vm);

// Once per frame after the object tree ticked: the listener follows its tracked camera, status changes of the sounds'
// modules start and stop them (gsSound::statusUpdated), world voices of disabled or inaudible grids stop, the voice
// mixer updates; REGS_DEBUG_AUDIO logs the voice count once per second.
void soundsFrame(script::VM& vm, float dt);
// At exit (gsFreeSounds): every voice stops, the device closes, the capture file is finished.
void soundsShutdown();

bool soundsEnabled();                       // enable_sounds succeeded
bool soundListener(render::Vec3& eye);      // the listener's position (false before the first listener update)
bool soundDebug();                          // REGS_DEBUG_AUDIO

// A template gsSound (vehicle, item, character model sounds; $passengerSound) as the engine clones it
// (createInstance, §11): its sound, mode and parameters.
struct SoundTemplate {
  audio::ClipRef clip;
  bool threeD = true;
  float volume = 1, pitch = 1, minDistance = 10, maxDistance = 100;
  std::string path;
};
// false when o is no gsSound or has no loaded sound
bool soundTemplate(script::Object* o, SoundTemplate& out);

// An engine clone under a grid: plays the template at a world position (forced: head-relative at the listener, full
// volume, centred, §11.1), channel volume template.volume · volumeMult, rate template.pitch · pitchMult. Returns the
// voice (0 when nothing plays). The voice stops on its own when the grid is disabled or inaudible (statusUpdated).
int worldSoundPlay(script::Object* grid, const SoundTemplate& t, const render::Vec3& pos, const render::Vec3& vel,
                   bool forced, bool loop, float pitchMult, float volumeMult, const char* what);
void worldSoundUpdate(int voice, const SoundTemplate& t, const render::Vec3& pos, const render::Vec3& vel, bool forced,
                      float pitchMult, float volumeMult);
bool worldSoundAlive(int voice);
void worldSoundStop(int voice, const char* why);

}  // namespace regs::engine
