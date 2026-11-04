#include "engine/engine.h"
#include "engine/grid_save.h"
#include "engine/interaction.h"
#include "engine/grid_edit.h"
#include "engine/timeline.h"
#include "engine/minimap.h"
#include "engine/skinning.h"
#include "engine/sim.h"

#include <cstdio>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/display.h"
#include "engine/gui.h"
#include "engine/scene3d.h"
#include "engine/shaders.h"
#include "engine/tools.h"
#include "engine/grid.h"
#include "engine/host.h"

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
  vfs_.rescan();
  vm_ = std::make_unique<VM>(vfs_);
  registerGeneratedApi(*vm_);
  registerCoreNatives(*vm_);
  registerObjectNatives(*vm_);
  registerUiNatives(*vm_);
  registerDisplayNatives(*vm_);
  registerInputNatives(*vm_);
  registerSceneNatives(*vm_);
  registerSkinningNatives(*vm_);
  registerShaderNatives(*vm_);
  registerGridNatives(*vm_);
  registerMinimapNatives(*vm_);
  registerInteractionNatives(*vm_);
  registerToolNatives(*vm_);
  registerSaveNatives(*vm_);
  registerTimelineNatives(*vm_);
  registerGridEditNatives(*vm_);
  registerSimNatives(*vm_);
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

Engine::~Engine() = default;

bool Engine::init(const std::u16string& initScript) {
  // the -i start-up file is compiled into MAIN itself (semantics.md §6)
  LoadOptions opt;
  opt.into = main_;
  Value r = vm_->load(initScript, opt);
  return r.object() != nullptr && main_->code;
}

void Engine::frame(float dt) {
  ++frames_;
  if (Display* d = display()) d->advanceEffectTimer(dt);
  // gsMain.tick (0x100a0700): script threads first, then the enabled children in order
  vm_->runThreads(dt);
  vm_->tickChildren(main_.get(), dt);
}

}  // namespace regs::engine
