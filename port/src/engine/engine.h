#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "script/vm.h"
#include "vfs/vfs.h"

namespace regs::engine {

// The engine instance: VFS, script VM with the full native API, and the root object MAIN (gsMain).
// Mirrors gsInitialize("GSystem.dll -i <init script>") / gsMainLoop / gsDestroy of the original loader.
class Engine {
 public:
  Engine(const std::filesystem::path& gameDir, const std::filesystem::path& documents);
  ~Engine();

  // Runs the init script (metro/scripts/init.script for the game).
  bool init(const std::u16string& initScript);
  // One frame: script threads, then the object tree (dt in seconds, already clamped).
  void frame(float dt);

  script::VM& vm() { return *vm_; }
  std::function<void(const std::string&)> echo;    // host side of the script output sink
  std::function<void(const std::string&)> errors;  // host side of the error sink
  Vfs& vfs() { return vfs_; }
  script::Object* main() { return main_.get(); }
  uint64_t frames() const { return frames_; }

 private:
  Vfs vfs_;
  std::unique_ptr<script::VM> vm_;
  script::Ref<script::Object> main_;
  uint64_t frames_ = 0;
};

}  // namespace regs::engine
