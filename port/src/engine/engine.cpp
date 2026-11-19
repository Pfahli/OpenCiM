#include "engine/engine.h"
#include "engine/effects.h"
#include "engine/grid_save.h"
#include "engine/interaction.h"
#include "engine/grid_edit.h"
#include "engine/timeline.h"
#include "engine/minimap.h"
#include "engine/skinning.h"
#include "engine/sky.h"
#include "engine/sim.h"
#include "engine/particles.h"
#include "engine/traffic_maps.h"
#include "engine/snowplows.h"
#include "engine/sound.h"

#include <chrono>
#include <cstdio>

#include "core/env.h"
#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/display.h"
#include "engine/gui.h"
#include "engine/scene3d.h"
#include "engine/shaders.h"
#include "engine/tools.h"
#include "engine/tool_objects.h"
#include "engine/tool_addstop.h"
#include "engine/tool_templates.h"
#include "engine/osm.h"
#include "engine/tool_terrain.h"
#include "engine/tool_tracks.h"
#include "engine/ui_graphs.h"
#include "engine/grid.h"
#include "engine/host.h"
#include "engine/user_folders.h"
#include "engine/mods.h"
#include "engine/popout.h"
#include "engine/terrain.h"

namespace regs::engine {

using namespace regs::script;

Host& host() {
  static Host h;
  return h;
}

Engine::Engine(const std::filesystem::path& gameDir, const std::filesystem::path& documents) {
  host().documents = documents;
  vfs_.setInstallRoot(gameDir);
  if (!host().dataDir.empty()) vfs_.setOverlayRoot(host().dataDir);
  vfs_.setCommunityContent(host().communityAddons);
  vm_ = std::make_unique<VM>(vfs_);
  loadMods(vfs_, *vm_);  // enabled mods: VFS layers and script extensions (engine/mods.cpp)
  vfs_.rescan();
  registerGeneratedApi(*vm_);
  registerCoreNatives(*vm_);
  registerObjectNatives(*vm_);
  registerSoundNatives(*vm_);
  registerUiNatives(*vm_);
  registerGraphNatives(*vm_);
  registerDisplayNatives(*vm_);
  registerInputNatives(*vm_);
  registerSceneNatives(*vm_);
  registerPopoutNatives(*vm_);
  registerSkyNatives(*vm_);
  registerParticleNatives(*vm_);
  registerTrafficMapNatives(*vm_);
  registerSnowplowNatives(*vm_);
  registerSkinningNatives(*vm_);
  registerShaderNatives(*vm_);
  registerGridNatives(*vm_);
  registerMinimapNatives(*vm_);
  registerInteractionNatives(*vm_);
  registerToolNatives(*vm_);
  registerObjectTools(*vm_);
  registerAddStopTool(*vm_);
  registerTrackTools(*vm_);
  registerTerrainTools(*vm_);
  registerTemplateTools(*vm_);
  registerOsmNatives(*vm_);
  registerStylePreviewNatives(*vm_);
  registerCurveTool(*vm_);
  registerSaveNatives(*vm_);
  registerTimelineNatives(*vm_);
  registerGridEditNatives(*vm_);
  registerSimNatives(*vm_);
  registerUserFolderNatives(*vm_);
  registerModNatives(*vm_);
  registerTerrainNatives(*vm_);
  // MAIN: the root gsMain object, registered as a global when it is attached (gsMain setParent, 0x100a15c0)
  main_ = vm_->newObject(vm_->findClass("gsMain"), nullptr);
  vm_->setGlobal(u"MAIN", Value::heap(VT::Object, main_.get()));
  // output and error sinks (0x100a14e0 / 0x100a1400): print, then fire MAIN.onInfoMsg / onErrorMsg; the engine shows
  // a message box only when no handler is bound
  vm_->output = [this](const std::string& s) {
    if (echo) echo(s);
    vm_->fireEvent(main_.get(), u"onInfoMsg", {Value::string(utf8ToUtf16(s))});
  };
  vm_->errorOutput = [this](const std::string& s) {
    if (errors) errors(s);
    vm_->fireEvent(main_.get(), u"onErrorMsg", {Value::string(utf8ToUtf16(s))});
  };
}

Engine::~Engine() { soundsShutdown(); }

bool Engine::init(const std::u16string& initScript) {
  // the -i start-up file is compiled into MAIN itself (semantics.md §6)
  LoadOptions opt;
  opt.into = main_;
  Value r = vm_->load(initScript, opt);
  return r.object() != nullptr && main_->code;
}

void Engine::frame(float dt) {
  ++frames_;
  vfs_.finishWrites();  // packages written in the background since the last frame mount now (Vfs::writePackage)
  if (Display* d = display()) d->advanceEffectTimer(dt);
  // REGS_LOG_TIME (measurements): ticks over 250 ms are logged with the script threads' share (an autosave, the
  // save of REGS_DEBUG_SAVE_AT); the render's own slow frames are logged by Display::renderFrame
  static const bool logTime = regs::envVar("REGS_LOG_TIME") != nullptr;
  const auto t0 = std::chrono::steady_clock::now();
  // gsMain.tick (0x100a0700): script threads first, then the enabled children in order
  vm_->runThreads(dt);
  const auto t1 = std::chrono::steady_clock::now();
  vm_->tickChildren(main_.get(), dt);
  soundsFrame(*vm_, dt);  // listener, sound status, mixer update
  if (logTime) {
    const auto t2 = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t2 - t0).count();
    if (ms > 250)
      LOG_INFO("slow tick: %.0f ms at frame %llu (script threads %.0f ms, children %.0f ms)", ms, (unsigned long long)frames_,
               std::chrono::duration<double, std::milli>(t1 - t0).count(), std::chrono::duration<double, std::milli>(t2 - t1).count());
  }
}

}  // namespace regs::engine
