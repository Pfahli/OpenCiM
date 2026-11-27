// GUI natives the line and vehicle panels need (docs/ui/lines.md §4): the gauge bars of the vehicle lists
// (gsGaugeCtrl), and rendering a 3D preview into a texture on demand (gsVideo_OGL.renderViewport,
// gsTextureTarget_OGL.resetTexture), which the vehicle shop's turning models use (metro/ui/objectlist/renderer.script).
#include <algorithm>
#include <cstdlib>
#include <string>

#include "engine/api.h"
#include "core/env.h"
#include "engine/display.h"
#include "engine/gui.h"
#include "engine/scene3d.h"

namespace regs::engine {

using namespace regs::script;

namespace {

// gsGaugeCtrl (L:0x081cf040 move): every tick its width is offset + arg0 · multiplier (multiplier +0x19c, ctor 1;
// offset +0x1a0, ctor 0), arg 0 being whatever setArg gave it (a value, an @reference or a macro)
class GaugeObj : public ControlObj {
 public:
  using ControlObj::ControlObj;
  void tick(VM& vm, float dt) override {
    Value a = args.empty() ? Value() : vm.resolve(args[0]);
    if (a.type() == VT::Int || a.type() == VT::Float || a.type() == VT::Bool) {
      auto num = [&](const char16_t* n, float def) {
        auto it = fieldValues.find(n);
        return it == fieldValues.end() || it->second.isNone() ? def : vm.toFloat(vm.resolve(it->second));
      };
      float w = num(u"offset", 0.0f) + vm.toFloat(a) * num(u"multiplier", 1.0f);
      if (w != width) setRect(vm, left, top, w, height);
    }
    ControlObj::tick(vm, dt);
  }
};

}  // namespace

void registerLinePanelNatives(VM& vm) {
  if (ClassInfo* c = vm.findClass("gsGaugeCtrl")) {
    c->factory = [](const ClassInfo* ci) { return Ref<Object>(new GaugeObj(ci)); };
    if (!c->fields.count(u"offset")) addField(c, u"offset", 4, false);
  }
  // gs3DCamera minDistance / maxDistance (+0x140 / +0x144): the constructor's −1 (L:0x080e9c00) = "the viewport's";
  // a camera no script configured (the shop's preview camera) must not clip everything at 0
  if (ClassInfo* c = vm.findClass("gs3DCamera"))
    for (const char16_t* name : {u"minDistance", u"maxDistance"})
      if (auto f = c->fields.find(name); f != c->fields.end())
        f->second.get = [key = std::u16string(name)](VM&, Object* o) {
          auto it = o->fieldValues.find(key);
          return it == o->fieldValues.end() || it->second.isNone() ? Value::real(-1.0f) : it->second;
        };
  // gsGridObject width / height / length (+0x1fc / +0x200 / +0x208): the constructor's 1 (L:0x08186210) for templates
  // whose script leaves them alone (vehicles); the shop's preview camera distance and height come from them
  if (ClassInfo* c = vm.findClass("gsGridObject"))
    for (const char16_t* name : {u"width", u"height", u"length"})
      if (auto f = c->fields.find(name); f != c->fields.end())
        f->second.get = [key = std::u16string(name)](VM&, Object* o) {
          auto it = o->fieldValues.find(key);
          return it == o->fieldValues.end() || it->second.isNone() ? Value::integer(1) : it->second;
        };
  // gsViewPort clearColor: the gsViewport_OGL constructor's 0xFF000000 (L:0x08204470.c:42), so a viewport no script
  // gave a colour clears its colour buffer (to transparent black, scene-basics.md §7); the shop's turning preview
  // otherwise leaves trails
  // gsViewPort minDistance / maxDistance: the gsViewport_OGL constructor's 0.1 / 400 (+0x54 / +0x58, L:0x08204470).
  // Unset they read 0, a near plane at 0 leaves no depth precision: the shop's previews showed the vehicles' insides
  // through their walls (owner's report 2026-11-25)
  if (ClassInfo* c = vm.findClass("gsViewPort"))
    for (auto [name, def] : {std::pair{u"minDistance", 0.1f}, std::pair{u"maxDistance", 400.0f}})
      if (auto f = c->fields.find(name); f != c->fields.end())
        f->second.get = [key = std::u16string(name), def = def](VM&, Object* o) {
          auto it = o->fieldValues.find(key);
          return it == o->fieldValues.end() || it->second.isNone() ? Value::real(def) : it->second;
        };
  if (ClassInfo* c = vm.findClass("gsViewPort"))
    if (auto f = c->fields.find(u"clearColor"); f != c->fields.end())
      f->second.get = [](VM&, Object* o) {
        auto it = o->fieldValues.find(u"clearColor");
        return it == o->fieldValues.end() || it->second.isNone() ? Value::integer(int32_t(0xFF000000u)) : it->second;
      };
  // renderViewport(viewport) (gsVideo_OGL): the viewport's scene drawn into its texture target now, outside the frame
  implement(vm, "gsVideo_OGL", u"renderViewport", [](VM& v, Object*, int, const Value* a) {
    auto* vp = dynamic_cast<ControlObj*>(v.resolve(a[0]).object());
    Display* d = display();
    if (!d || !vp || vp->destroyed) return Value();
    renderViewportScene(*d, v, vp);
    // debugging: REGS_DEBUG_PREVIEW=<file stem> saves the first previews (<stem>-<n>.png, rows bottom first)
    if (const char* stem = regs::envVar("REGS_DEBUG_PREVIEW")) {
      static int n = 0;
      auto fx = vp->fieldValues.find(u"<effects>");
      if (n < 8 && fx != vp->fieldValues.end() && fx->second.array())
        for (const Value& e : fx->second.array()->items)
          if (Object* eo = e.object(); eo && eo->cls->name == "gsTextureTarget_OGL")
            if (auto t = eo->fieldValues.find(u"<texture>"); t != eo->fieldValues.end())
              if (auto* tex = dynamic_cast<TextureObj*>(t->second.object()))
                if (render::RenderTarget* rt = d->renderTarget(tex, 0, 0)) d->dumpTarget(*rt, std::string(stem) + "-" + std::to_string(n++) + ".png");
    }
    return Value();
  });
  // resetTexture() (gsTextureTarget_OGL): the target draws into a new texture of the same size from now on, so the
  // image drawn before stays with whoever shows it; returns the new texture
  implement(vm, "gsTextureTarget_OGL", u"resetTexture", [](VM& v, Object* s, int, const Value*) {
    auto it = s->fieldValues.find(u"<texture>");
    auto* old = it == s->fieldValues.end() ? nullptr : dynamic_cast<TextureObj*>(it->second.object());
    Ref<Object> t = v.newObject(v.findClass("gsTextureVariant_OGL"), nullptr);
    if (auto* tex = dynamic_cast<TextureObj*>(t.get())) {
      tex->width = old ? old->width : 256;
      tex->height = old ? old->height : 256;
      tex->path = u"<render target>";
      tex->renderTarget = true;
    }
    s->fieldValues[u"<texture>"] = Value::heap(VT::Object, t.get());
    return Value::heap(VT::Object, t.get());
  });
}

}  // namespace regs::engine
