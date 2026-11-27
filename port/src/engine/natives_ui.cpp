// Window, video, texture and GUI-control natives: geometry, images, borders, text, formatting and fades behave like
// the engine (docs/ui/controls.md); the display draws from this state (engine/display.cpp).
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/effects.h"
#include "engine/font.h"
#include "engine/gui.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::script;

namespace {

Value plainOf(const Value& v) { return v.ref() ? v.ref()->get() : v; }
float num(VM& vm, const Value& v) { return vm.toFloat(v); }

// ---------------------------------------------------------------------------------------------------------------
// textures: cached by path; a missing or unreadable file gives the default texture (FUN_100431c0)

std::map<std::u16string, Ref<Object>>& textureCache() {
  static std::map<std::u16string, Ref<Object>> cache;
  return cache;
}

bool ddsSize(const std::vector<uint8_t>& d, int& w, int& h) {
  if (d.size() < 128 || d[0] != 'D' || d[1] != 'D' || d[2] != 'S' || d[3] != ' ') return false;
  auto u32 = [&](size_t at) { return int(uint32_t(d[at]) | d[at + 1] << 8 | d[at + 2] << 16 | uint32_t(d[at + 3]) << 24); };
  h = u32(12);
  w = u32(16);
  return w > 0 && h > 0;
}

}  // namespace

void forEachTexture(const std::function<void(TextureObj*)>& fn) {
  for (auto& [path, t] : textureCache())
    if (auto* tex = dynamic_cast<TextureObj*>(t.get())) fn(tex);
}

Object* textureIfLoaded(const std::u16string& path) {
  auto it = textureCache().find(path);
  return it == textureCache().end() ? nullptr : it->second.get();
}

Ref<Object> getTexture(VM& vm, std::u16string path) {
  if (path.empty()) path = u"textures/default.dds";
  auto& cache = textureCache();
  if (auto it = cache.find(path); it != cache.end()) return it->second;
  std::vector<uint8_t> data;
  int w = 0, h = 0;
  // the size from the 128-byte header only: the upload reads the whole file (Display::texture)
  if (!vm.vfs().readHead(utf16ToUtf8(path), data, 128) || !ddsSize(data, w, h)) {
    if (path != u"textures/default.dds") return getTexture(vm, u"textures/default.dds");
    w = h = 1;
  }
  Ref<Object> t = vm.newObject(vm.findClass("gsTextureVariant_OGL"), nullptr);
  auto* tex = dynamic_cast<TextureObj*>(t.get());
  if (!tex) vm.error("internal: texture class missing");
  tex->path = path;
  tex->width = w;
  tex->height = h;
  cache[path] = t;
  return t;
}

// ---------------------------------------------------------------------------------------------------------------
// GUI controls (gsControl, docs/ui/controls.md). Items (image, border, text, client area) are not retained objects
// here: the display draws them from this state every frame, which gives the same result as the engine's rebuilds.

namespace {

ControlObj* ownerOf(const ControlObj* c) { return dynamic_cast<ControlObj*>(c->parent); }

bool effectivelyVisible(const ControlObj* c) {
  for (; c; c = ownerOf(c))
    if (!c->visible()) return false;
  return true;
}

}  // namespace

uint32_t mulA(uint32_t argb, float v) {
  // colour with its alpha scaled by v, truncated (FUN_10074ef0)
  return (argb & 0x00FFFFFFu) | (uint32_t(int32_t(float(argb >> 24) * v)) & 0xFFu) << 24;
}

float ControlObj::desktopLeft() const {
  const ControlObj* p = ownerOf(this);
  return p ? p->marginL + p->desktopLeft() + left : left;
}

float ControlObj::desktopTop() const {
  const ControlObj* p = ownerOf(this);
  return p ? p->marginT + p->desktopTop() + top : top;
}

bool ControlObj::clipped() const {
  const ControlObj* p = ownerOf(this);
  return p && (left >= p->width || top >= p->height || left + width <= 0 || top + height <= 0);
}

void ControlObj::setRect(VM& vm, float l, float t, float w, float h) {
  bool changed = l != left || t != top || w != width || h != height;
  left = l;
  top = t;
  width = w;
  height = h;
  if (changed) vm.fireEvent(this, u"onChangeRect", {});
}

void ControlObj::setText(VM&, const std::u16string& s) {
  raw = display = s;
  if (formatEnabled) {
    hasCodes = false;
    bool sawEscape = false;
    for (size_t i = 0; i < raw.size(); ++i) {
      if (raw[i] != u'%') continue;
      if (i + 1 < raw.size() && raw[i + 1] != u'%') {
        hasCodes = true;
        break;
      }
      sawEscape = true;
      ++i;
    }
    if (!hasCodes && sawEscape)  // static text: "%%" becomes "%", a lone trailing '%' disappears
      for (size_t i = 0; i < display.size(); ++i)
        if (display[i] == u'%') display.erase(i, 1);
  }
  hasText = true;
  if (hasCodes) display.clear();  // formatted on the next tick
}

void ControlObj::tick(VM& vm, float dt) {
  if (active && isEdit()) {  // caret blink (L:0x081d4770): phase += blinkSpeed·dt, wrapped into −1..1
    auto b = fieldValues.find(u"blinkSpeed");
    caretPhase += (b == fieldValues.end() || b->second.isNone() ? 2.0f : vm.toFloat(b->second)) * dt;
    while (caretPhase > 1) caretPhase -= 2;
  }
  if (hasCodes && effectivelyVisible(this)) {
    display = formatPattern(vm, raw, args.data(), 10);
    if (maxTextLength != -1 && int(display.size()) > maxTextLength) display.erase(size_t(std::max(0, maxTextLength)));
  }
  if (blending) {
    float v = dt * blendRate + visibility;
    if ((blendRate > 0 && v >= blendTarget) || (blendRate < 0 && v <= blendTarget)) {
      blending = false;
      v = blendTarget;
      if (blendTarget == 0) flags &= ~2u;  // auto-hide at the end of a fade-out
    }
    visibility = v;
    shadowPushed = true;  // setVisibility pushes the shadow colour to the text item
  }
  vm.tickChildren(this, dt);
}

std::u16string formatPattern(VM& vm, const std::u16string& p, const Value* args, int argc) {
  // gsControl::formatText (0x100a9cb0), controls.md §9.2
  std::u16string out;
  auto arg = [&](int i) -> Value { return i >= 0 && i < argc ? vm.resolve(args[i]) : Value(); };
  auto present = [](const Value& v) { return v.type() != VT::None && !v.isNull(); };
  for (size_t i = 0; i < p.size(); ++i) {
    char16_t c = p[i];
    if (c != u'%') {
      out += c;
      continue;
    }
    if (++i >= p.size()) break;
    char16_t d = p[i];
    if (d >= u'0' && d <= u'9') {
      Value v = arg(d - u'0');
      if (v.type() == VT::Float) {
        char buf[64];
        std::snprintf(buf, sizeof buf, "%#.3f", double(v.f()));
        out += utf8ToUtf16(buf);
      } else if (present(v)) {
        out += v.toString();
      }
    } else if (d == u'!') {
      if (i + 2 >= p.size()) break;
      Value v = arg((p[i + 1] - u'0') * 10 + (p[i + 2] - u'0'));
      if (present(v)) out += v.toString();
      i += 2;
    } else if (d == u'i' || d == u'f') {
      char16_t th = 0, decSep = 0;
      int pad = 0, decimals = d == u'f' ? 3 : 0;
      bool decGiven = false;
      size_t j = i + 1;
      for (;;) {
        if (j >= p.size()) return out;  // the pattern ends inside a code: the rest is dropped
        if (d == u'i' && p[j] == u't') {
          if (j + 1 >= p.size()) return out;
          th = p[j + 1];
          j += 2;
        } else if (d == u'i' && p[j] == u'p') {
          if (j + 1 >= p.size()) return out;
          pad = p[j + 1] - u'0';
          j += 2;
        } else if (p[j] == u'd') {
          if (j + 2 >= p.size()) return out;
          decSep = p[j + 1];
          decimals = p[j + 2] - u'0';
          decGiven = true;
          j += 3;
        } else {
          break;
        }
      }
      i = j;
      Value v = arg(p[j] - u'0');
      if (!present(v)) continue;
      if (d == u'f') {
        // the engine builds this "%#.Nf" wrongly in both builds; this is what it meant (§9.2)
        char buf[64];
        std::snprintf(buf, sizeof buf, "%#.*f", decimals, double(vm.toFloat(v)));
        std::u16string s = utf8ToUtf16(buf);
        if (decGiven)
          for (auto& ch : s)
            if (ch == u'.') ch = decSep;
        out += s;
        continue;
      }
      int64_t val = vm.toInt(v);
      uint64_t n = uint64_t(val < 0 ? -val : val);
      std::u16string s;
      int pos = 1, dec = decGiven ? decimals + 1 : 0, grp = dec;
      uint64_t q;
      do {
        q = n / 10;
        s.insert(s.begin(), char16_t(u'0' + n % 10));
        ++pos;
        n = q;
        if (pos == dec) {
          s.insert(s.begin(), decSep);
          ++pos;
          if (q == 0) dec = grp = dec + 1;
        } else if ((pos - grp) % 4 == 0 && q != 0 && th) {
          s.insert(s.begin(), th);
          ++pos;
        }
      } while (q != 0 || pos <= grp);
      if (val < 0) {
        s.insert(s.begin(), u'-');
        ++pos;
      }
      while (pos <= pad) {  // the zeros go in front of the sign
        s.insert(s.begin(), u'0');
        ++pos;
      }
      out += s;
    } else {
      out += d;  // "%%" and unknown codes give the character itself
    }
  }
  return out;
}

Font* controlFont(VM& vm, ControlObj* c) { return getFont(vm, c->fontName, c->fontExtra); }

void controlTextPos(ControlObj* c, const Font& f, TextParams& p, float& x, float& y) {
  // updateTextPos (0x100a85e0, §6.2)
  p.sizeX = c->sizeX;
  p.sizeY = c->sizeY;
  p.spacingX = c->spacingX;
  p.spacingY = c->spacingY;
  p.indent = 0;
  float L = c->desktopLeft(), T = c->desktopTop();
  float cW = c->width - c->marginL - c->marginR, cH = c->height - c->marginT - c->marginB;
  const std::u16string& s = c->display;
  if (c->textAlign & 0x20) {
    x = (L + c->width - c->marginR) - measureWidth(f, s, p);
    p.boxX = 10000;
  } else if (c->textAlign & 0x10) {
    double d = (double(cW) - measureWidth(f, s, p)) * 0.5;
    if (c->sizeX >= 4.0f) d = std::trunc(d);
    x = float(d + c->marginL + L);
    p.boxX = 10000;
  } else {
    x = c->marginL + L;
    p.boxX = cW;  // left-aligned text wraps at the client width
  }
  if (c->textAlign & 4) {
    y = (T + c->height - c->marginB) - measureHeight(f, s, p);
  } else if (c->textAlign & 2) {
    double d = (double(cH) - measureHeight(f, s, p)) * 0.5;
    if (c->sizeY >= 4.0f) d = std::trunc(d);
    y = float(d + c->marginT + T);
  } else {
    y = c->marginT + T;
  }
}

namespace {

void floatField(ClassInfo* c, const char16_t* name, std::function<float&(ControlObj*)> ref, bool rect) {
  FieldInfo& f = c->fields[name];
  f.name = name;
  f.type = 4;
  f.get = [ref](VM&, Object* o) { return Value::real(ref(static_cast<ControlObj*>(o))); };
  f.set = [ref, rect](VM& vm, Object* o, const Value& v) {
    auto* ctl = static_cast<ControlObj*>(o);
    float nv = vm.toFloat(v);
    if (ref(ctl) == nv) return;
    ref(ctl) = nv;
    if (rect) vm.fireEvent(ctl, u"onChangeRect", {});
  };
}

void roField(ClassInfo* c, const char16_t* name, std::function<Value(ControlObj*)> get) {
  FieldInfo& f = c->fields[name];
  f.name = name;
  f.readOnly = true;
  f.get = [get](VM&, Object* o) { return get(static_cast<ControlObj*>(o)); };
}

void rwField(ClassInfo* c, const char16_t* name, int type, std::function<Value(ControlObj*)> get,
             std::function<void(VM&, ControlObj*, const Value&)> set) {
  FieldInfo& f = c->fields[name];
  f.name = name;
  f.type = type;
  f.get = [get](VM&, Object* o) { return get(static_cast<ControlObj*>(o)); };
  f.set = [set](VM& vm, Object* o, const Value& v) { set(vm, static_cast<ControlObj*>(o), v); };
}

ControlObj* ctl(Object* o) { return static_cast<ControlObj*>(o); }
uint32_t argb(VM& vm, const Value& v) { return uint32_t(vm.toInt(v)); }

}  // namespace

void registerUiNatives(VM& vm) {
  // ---- textures ----
  if (ClassInfo* c = vm.findClass("gsTextureVariant_OGL")) {
    c->factory = [](const ClassInfo* ci) { return Ref<Object>(new TextureObj(ci)); };
    for (const char16_t* n : {u"width", u"height"}) {
      FieldInfo& f = c->fields[n];
      f.name = n;
      f.readOnly = true;
      bool w = n[0] == 'w';
      f.get = [w](VM&, Object* o) { auto* t = static_cast<TextureObj*>(o); return Value::integer(w ? t->width : t->height); };
    }
  }
  // saveToFile(path) (gsTextureVariant_OGL::method_saveToFile L:0x081fba50): ".bmp" a 24-bit BMP of level 0 (rows as
  // the texture has them, so the image is upside down in a viewer, as the original's); anything else a DDS A8R8G8B8
  // (flags 0x21007, caps 0x40100a) with the whole mip chain, each level the 2×2 mean of the one before, (a+b+c+d+3)>>2
  // over SIGNED bytes as the original's char arithmetic (bright channels wrap; faithful). The editor saves the
  // minimap this way as <map>.dds. Only textures with CPU pixels (renderMap) can be saved.
  implement(vm, "gsTextureVariant_OGL", u"saveToFile", [](VM& v, Object* s, int, const Value* a) {
    auto* t = dynamic_cast<TextureObj*>(s);
    std::string path = utf16ToUtf8(v.toStr(a[0]));
    if (!t || t->width <= 0 || t->height <= 0 || t->cpuBGRA.size() < size_t(t->width) * size_t(t->height) * 4) {
      LOG_WARN("texture saveToFile %s: no pixels on the CPU (only renderMap textures can be saved)", path.c_str());
      return Value();
    }
    int w = t->width, h = t->height;
    std::vector<uint8_t> out;
    auto p32 = [&](uint32_t x) { out.insert(out.end(), reinterpret_cast<uint8_t*>(&x), reinterpret_cast<uint8_t*>(&x) + 4); };
    auto p16 = [&](uint16_t x) { out.insert(out.end(), reinterpret_cast<uint8_t*>(&x), reinterpret_cast<uint8_t*>(&x) + 2); };
    std::string ext = path.size() >= 4 ? path.substr(path.size() - 4) : "";
    for (char& ch : ext) ch = char(std::tolower(uint8_t(ch)));
    if (ext == ".bmp") {
      p16(0x4D42);
      p32(uint32_t(w * h * 3 + 0x38));
      p32(0);
      p32(0x38);
      p32(0x28);
      p32(uint32_t(w));
      p32(uint32_t(h));
      p16(1);
      p16(24);
      p32(0);
      p32(uint32_t(w * h * 3));
      p32(0x400);
      p32(0x400);
      p32(0);
      p32(0);
      for (size_t i = 0; i < size_t(w) * size_t(h); ++i) out.insert(out.end(), &t->cpuBGRA[i * 4], &t->cpuBGRA[i * 4] + 3);
      return Value::boolean(v.vfs().write(path, out));
    }
    int mips = 1;
    for (int m = std::max(w, h) >> 1; m; m >>= 1) ++mips;
    p32(0x20534444);  // "DDS "
    p32(0x7C);
    p32(0x21007);
    p32(uint32_t(h));
    p32(uint32_t(w));
    p32(0);
    p32(0);
    p32(uint32_t(mips));
    for (int i = 0; i < 11; ++i) p32(0);
    p32(0x20);
    p32(0x41);
    p32(0);
    p32(0x20);
    p32(0xFF0000);
    p32(0xFF00);
    p32(0xFF);
    p32(0xFF000000u);
    p32(0x40100A);
    for (int i = 0; i < 4; ++i) p32(0);
    std::vector<uint8_t> level = t->cpuBGRA;
    int lw = w, lh = h;
    for (;;) {
      out.insert(out.end(), level.begin(), level.begin() + std::ptrdiff_t(size_t(lw) * size_t(lh) * 4));
      if (lw == 1 && lh == 1) break;
      int nw = std::max(1, lw >> 1), nh = std::max(1, lh >> 1);
      std::vector<uint8_t> next(size_t(nw) * size_t(nh) * 4);
      for (int y = 0; y < nh; ++y)
        for (int x = 0; x < nw; ++x)
          for (int c = 0; c < 4; ++c) {
            auto px = [&](int xx, int yy) {
              return int(int8_t(level[(size_t(std::min(yy, lh - 1)) * size_t(lw) + size_t(std::min(xx, lw - 1))) * 4 + size_t(c)]));
            };
            next[(size_t(y) * size_t(nw) + size_t(x)) * 4 + size_t(c)] =
                uint8_t((px(2 * x, 2 * y) + px(2 * x + 1, 2 * y) + px(2 * x, 2 * y + 1) + px(2 * x + 1, 2 * y + 1) + 3) >> 2);
          }
      level.swap(next);
      lw = nw;
      lh = nh;
    }
    return Value::boolean(v.vfs().write(path, out));
  });
  for (const char* video : {"gsVideo_OGL", "gsVideo"}) {
    implement(vm, video, u"new_texture", [](VM& v, Object*, int argc, const Value* a) {
      std::u16string path = argc > 0 ? v.resolvePath(v.toStr(a[0]), nullptr) : u"";
      return Value::heap(VT::Object, getTexture(v, path).get());
    });
    // render-to-texture target (post effect): owns a texture of the requested size, returned by getTexture()
    // post effects: the objects scripts configure (shadow map, bloom, shader effect), run by the viewport's 3D pass;
    // the size arguments (shadow map width/height) are kept for it
    for (auto [m, cls] : {std::pair{u"new_shadowEffect", "gsShadowEffect_OGL"}, std::pair{u"new_bloomEffect", "gsBloomEffect_OGL"},
                          std::pair{u"new_shaderEffect", "gsShaderEffect_OGL"}})
      implement(vm, video, m, [cls = cls](VM& v, Object*, int argc, const Value* a) {
        Ref<Object> e = v.newObject(v.findClass(cls), nullptr);
        if (argc > 0) e->fieldValues[u"<width>"] = Value::integer(v.toInt(a[0]));
        if (argc > 1) e->fieldValues[u"<height>"] = Value::integer(v.toInt(a[1]));
        initEffectFields(e.get());
        return Value::heap(VT::Object, e.get());
      });
    implement(vm, video, u"new_textureTarget", [](VM& v, Object*, int argc, const Value* a) {
      Ref<Object> target = v.newObject(v.findClass("gsTextureTarget_OGL"), nullptr);
      Ref<Object> t = v.newObject(v.findClass("gsTextureVariant_OGL"), nullptr);
      if (auto* tex = dynamic_cast<TextureObj*>(t.get())) {
        tex->width = argc > 0 ? v.toInt(a[0]) : 256;
        tex->height = argc > 1 ? v.toInt(a[1]) : 256;
        tex->path = u"<render target>";
        tex->renderTarget = true;
      }
      target->fieldValues[u"<texture>"] = Value::heap(VT::Object, t.get());
      return Value::heap(VT::Object, target.get());
    });
  }
  implement(vm, "gsTextureTarget_OGL", u"getTexture", [](VM&, Object* s, int, const Value*) {
    auto it = s->fieldValues.find(u"<texture>");
    return it == s->fieldValues.end() ? Value() : it->second;
  });
  implement(vm, "gsTextureTarget_OGL", u"resetTexture", [](VM&, Object*, int, const Value*) { return Value(); });
  // viewport post effects: kept in order for the renderer
  implement(vm, "gsViewPort", u"addEffect", [](VM& v, Object* s, int, const Value* a) {
    Value& list = s->fieldValues[u"<effects>"];
    if (!list.array()) list = Value::heap(VT::Array, make<ArrayObj>().get());
    list.array()->items.push_back(plainOf(a[0]));
    return Value();
  });
  implement(vm, "gsViewPort", u"removeEffect", [](VM&, Object* s, int, const Value* a) {
    Value& list = s->fieldValues[u"<effects>"];
    if (ArrayObj* arr = list.array())
      std::erase_if(arr->items, [&](const Value& e) { return e.heapPtr() == plainOf(a[0]).heapPtr(); });
    return Value();
  });
  implement(vm, "gsViewPort", u"clearEffects", [](VM&, Object* s, int, const Value*) {
    s->fieldValues.erase(u"<effects>");
    return Value();
  });

  // ---- colours: color(grey) / color(rgb, alpha) / color(r, g, b [, a]) -> 0xAARRGGBB ----
  implementGlobal(vm, u"color", [](VM& v, Object*, int argc, const Value* a) {
    auto i = [&](int k) { return uint32_t(v.toInt(a[k])); };
    switch (argc) {
      case 1: { uint32_t g = i(0) & 0xFF; return Value::integer(int32_t(0xFF000000u | g << 16 | g << 8 | g)); }
      case 2: return Value::integer(int32_t(i(1) << 24 | (i(0) & 0xFFFFFF)));
      case 3: return Value::integer(int32_t(0xFF000000u | (i(0) & 0xFF) << 16 | (i(1) & 0xFF) << 8 | (i(2) & 0xFF)));
      case 4: return Value::integer(int32_t(i(3) << 24 | (i(0) & 0xFF) << 16 | (i(1) & 0xFF) << 8 | (i(2) & 0xFF)));
      default: return Value();
    }
  });

  // ---- controls ----
  ClassInfo* c = vm.findClass("gsControl");
  if (!c) return;
  c->factory = [](const ClassInfo* ci) { return Ref<Object>(new ControlObj(ci)); };
  floatField(c, u"left", [](ControlObj* o) -> float& { return o->left; }, true);
  floatField(c, u"top", [](ControlObj* o) -> float& { return o->top; }, true);
  floatField(c, u"width", [](ControlObj* o) -> float& { return o->width; }, true);
  floatField(c, u"height", [](ControlObj* o) -> float& { return o->height; }, true);
  roField(c, u"desktopLeft", [](ControlObj* o) { return Value::real(o->desktopLeft()); });
  roField(c, u"desktopTop", [](ControlObj* o) { return Value::real(o->desktopTop()); });
  roField(c, u"clientWidth", [](ControlObj* o) { return Value::real(o->width - o->marginL - o->marginR); });
  roField(c, u"clientHeight", [](ControlObj* o) { return Value::real(o->height - o->marginT - o->marginB); });
  roField(c, u"cropLeft", [](ControlObj* o) { return Value::real(o->cropL); });
  roField(c, u"cropTop", [](ControlObj* o) { return Value::real(o->cropT); });
  roField(c, u"cropWidth", [](ControlObj* o) { return Value::real(o->cropW); });
  roField(c, u"cropHeight", [](ControlObj* o) { return Value::real(o->cropH); });
  // writing visibility cancels a blend and pushes the colours (setVisibility, slot 0x16C)
  rwField(c, u"visibility", 4, [](ControlObj* o) { return Value::real(o->visibility); },
          [](VM& v, ControlObj* o, const Value& x) {
            o->blending = false;
            o->visibility = v.toFloat(x);
            o->shadowPushed = true;
          });
  // background sets both colours, hotBackground only the hot one; both create the image (0x100a6340/0x100a63d0)
  rwField(c, u"background", 3, [](ControlObj* o) { return Value::integer(int32_t(o->background)); },
          [](VM& v, ControlObj* o, const Value& x) {
            o->background = o->hotBackground = argb(v, x);
            o->hasImage = true;
          });
  rwField(c, u"hotBackground", 3, [](ControlObj* o) { return Value::integer(int32_t(o->hotBackground)); },
          [](VM& v, ControlObj* o, const Value& x) {
            o->hotBackground = argb(v, x);
            o->hasImage = true;
          });
  rwField(c, u"text", 6, [](ControlObj* o) { return Value::string(o->raw); },
          [](VM& v, ControlObj* o, const Value& x) { o->setText(v, v.toStr(x)); });
  rwField(c, u"textAlign", 3, [](ControlObj* o) { return Value::integer(int32_t(o->textAlign)); },
          [](VM& v, ControlObj* o, const Value& x) { o->textAlign = uint32_t(v.toInt(x)); });
  rwField(c, u"maxTextLength", 3, [](ControlObj* o) { return Value::integer(o->maxTextLength); },
          [](VM& v, ControlObj* o, const Value& x) { o->maxTextLength = v.toInt(x); });

  implement(vm, "gsControl", u"setRect", [](VM& v, Object* s, int, const Value* a) {
    ctl(s)->setRect(v, num(v, a[0]), num(v, a[1]), num(v, a[2]), num(v, a[3]));
    return Value();
  });
  implement(vm, "gsControl", u"setPosition", [](VM& v, Object* s, int, const Value* a) {
    auto* o = ctl(s);
    o->setRect(v, num(v, a[0]), num(v, a[1]), o->width, o->height);
    return Value();
  });
  implement(vm, "gsControl", u"setSize", [](VM& v, Object* s, int, const Value* a) {
    auto* o = ctl(s);
    o->setRect(v, o->left, o->top, num(v, a[0]), num(v, a[1]));
    return Value();
  });
  implement(vm, "gsControl", u"setMargins", [](VM& v, Object* s, int, const Value* a) {
    auto* o = ctl(s);
    o->marginL = num(v, a[0]);
    o->marginT = num(v, a[1]);
    o->marginR = num(v, a[2]);
    o->marginB = num(v, a[3]);
    return Value();
  });
  implement(vm, "gsControl", u"setCropRect", [](VM& v, Object* s, int, const Value* a) {
    auto* o = ctl(s);
    o->cropL = num(v, a[0]);
    o->cropT = num(v, a[1]);
    o->cropW = num(v, a[2]);
    o->cropH = num(v, a[3]);
    return Value();
  });
  implement(vm, "gsControl", u"setTiling", [](VM& v, Object* s, int, const Value* a) {
    ctl(s)->cropW = num(v, a[0]);
    ctl(s)->cropH = num(v, a[1]);
    return Value();
  });
  // setImage(path | texture | null, [slot]) (0x100a8ac0): binds material texture `slot`, colours to opaque white
  implement(vm, "gsControl", u"setImage", [](VM& v, Object* s, int argc, const Value* a) {
    auto* o = ctl(s);
    Value img = v.resolve(a[0]);
    int slot = argc > 1 ? v.toInt(a[1]) : 0;
    Ref<Object> tex;
    if (img.type() == VT::String) tex = getTexture(v, v.resolvePath(img.str(), s));
    else if (dynamic_cast<TextureObj*>(img.object())) tex = Ref<Object>(img.object());
    if (!tex && !o->hasMaterial) return Value();  // nothing to clear
    o->hasMaterial = true;
    if (slot == 0) o->texture = tex;
    else if (slot >= 1 && slot <= 7) o->slotTextures[slot - 1] = tex;
    o->background = o->hotBackground = 0xFFFFFFFF;
    o->hasImage = true;
    return Value();
  });
  implement(vm, "gsControl", u"setShader", [](VM& v, Object* s, int, const Value* a) {
    ctl(s)->shaderPath = v.resolvePath(v.toStr(a[0]), s);
    ctl(s)->hasMaterial = true;
    ctl(s)->hasImage = true;
    return Value();
  });
  implement(vm, "gsControl", u"setBorder", [](VM& v, Object* s, int argc, const Value* a) {
    auto* o = ctl(s);
    o->borderColor = argb(v, a[0]);
    o->borderWidth = num(v, a[1]);
    o->borderSides = argc == 3 ? uint32_t(v.toInt(a[2])) : 0xF;
    o->hasBorder = true;
    return Value();
  });
  // setFont(font, colour, sizeX, sizeY, [spacingX], [spacingY], [extra]) (0x100a9af0)
  implement(vm, "gsControl", u"setFont", [](VM& v, Object* s, int argc, const Value* a) {
    auto* o = ctl(s);
    o->fontName = v.toStr(a[0]);
    o->textColor = argb(v, a[1]);
    o->sizeX = num(v, a[2]);
    o->sizeY = num(v, a[3]);
    o->spacingX = argc > 4 ? num(v, a[4]) : 0.0f;
    o->spacingY = argc > 5 ? num(v, a[5]) : 0.0f;
    o->fontExtra = argc > 6 ? v.toStr(a[6]) : u"";
    o->hasText = true;
    return Value();
  });
  implement(vm, "gsControl", u"setTextShadow", [](VM& v, Object* s, int, const Value* a) {
    auto* o = ctl(s);
    o->shadowColor = argb(v, a[0]);
    o->shadowDX = num(v, a[1]);
    o->shadowDY = num(v, a[2]);
    o->shadowPushed = true;
    o->hasText = true;
    return Value();
  });
  // blend(target, duration) (0x100a7c50): a hidden control starts from 0; the control is shown
  implement(vm, "gsControl", u"blend", [](VM& v, Object* s, int, const Value* a) {
    auto* o = ctl(s);
    float target = num(v, a[0]), secs = num(v, a[1]);
    if (!o->visible()) o->visibility = 0;
    o->flags |= 2;
    o->blending = true;
    o->blendTarget = target;
    o->blendRate = (target - o->visibility) / secs;
    return Value();
  });
  implement(vm, "gsControl", u"setArg", [](VM& v, Object* s, int, const Value* a) {
    auto* o = ctl(s);
    int i = v.toInt(a[0]);
    if (i >= 0) {
      if (size_t(i) >= o->args.size()) o->args.resize(size_t(i) + 1);
      o->args[size_t(i)] = a[1];  // kept as given: '@' references and macros stay live
    }
    return Value();
  });
  implement(vm, "gsControl", u"getArg", [](VM& v, Object* s, int, const Value* a) {
    auto* o = ctl(s);
    int i = v.toInt(a[0]);
    return i >= 0 && size_t(i) < o->args.size() ? o->args[size_t(i)] : Value();
  });
  auto reorder = [](Object* s, bool front) {
    Object* p = s->parent;
    if (!p) return;
    auto& k = p->children;
    auto it = std::find_if(k.begin(), k.end(), [s](const Ref<Object>& r) { return r.get() == s; });
    if (it == k.end()) return;
    Ref<Object> keep = *it;
    k.erase(it);
    if (front) k.push_back(keep);
    else k.insert(k.begin(), keep);
  };
  implement(vm, "gsControl", u"sendToBack", [reorder](VM&, Object* s, int, const Value*) { reorder(s, false); return Value(); });
  implement(vm, "gsControl", u"bringToFront", [reorder](VM&, Object* s, int, const Value*) { reorder(s, true); return Value(); });
  // text metrics of the display string with the item's current box (0 without a text item)
  auto metric = [](bool height) {
    return [height](VM& v, Object* s, int, const Value*) {
      auto* o = ctl(s);
      if (!o->hasText) return Value::real(0);
      Font* f = controlFont(v, o);
      TextParams p;
      float x, y;
      controlTextPos(o, *f, p, x, y);
      return Value::real(height ? measureHeight(*f, o->display, p) : measureWidth(*f, o->display, p));
    };
  };
  implement(vm, "gsControl", u"getTextWidth", metric(false));
  implement(vm, "gsControl", u"getTextHeight", metric(true));
  implement(vm, "gsControl", u"visibleLines", [](VM&, Object* s, int, const Value*) {
    auto* o = ctl(s);
    return Value::integer(o->sizeY != 0 ? int32_t(std::floor(o->height / o->sizeY)) : 0);
  });
  implement(vm, "gsControl", u"getFormatIdAt", [](VM&, Object*, int, const Value*) { return Value::integer(-1); });
  if (ClassInfo* t = vm.findClass("gsTextSelCtrl"))  // no %-formatting in text-selection and edit controls
    t->factory = [](const ClassInfo* ci) {
      auto* o = new ControlObj(ci);
      o->formatEnabled = false;
      return Ref<Object>(o);
    };

  // formatText(text, colours): "{d:" (one digit) → U+0004 + 8 hex digits of colours[d], "}" → U+0005 (0x10075ad0)
  implementGlobal(vm, u"formatText", [](VM& v, Object*, int, const Value* a) {
    std::u16string in = v.toStr(a[0]), out;
    ArrayObj* colours = v.resolve(a[1]).array();
    for (size_t i = 0; i < in.size(); ++i) {
      char16_t ch = in[i];
      if (ch == u'}') {
        out += char16_t(5);
      } else if (ch == u'{') {
        if (i + 2 >= in.size() || in[i + 1] < u'0' || in[i + 1] > u'9' || in[i + 2] != u':')
          v.error(kErrInvalidArgs, "Invalid format text");
        size_t n = size_t(in[i + 1] - u'0');
        if (!colours || n >= colours->items.size()) v.error(kErrArrayIndex, "Array index out of bounds");
        char buf[16];
        std::snprintf(buf, sizeof buf, "%08X", uint32_t(v.toInt(colours->items[n])));
        out += char16_t(4);
        out += utf8ToUtf16(buf);
        i += 2;
      } else {
        out += ch;
      }
    }
    return Value::string(out);
  });

  // ---- window, desktop, module helpers ----
  implement(vm, "gsWindow", u"update", [](VM&, Object*, int, const Value*) { return Value(); });
  implement(vm, "gsModule", u"init", [](VM&, Object*, int, const Value*) { return Value(); });
  implement(vm, "gsModule", u"enable", [](VM&, Object* s, int, const Value*) { s->flags |= 1; return Value(); });
  implement(vm, "gsModule", u"disable", [](VM&, Object* s, int, const Value*) { s->flags &= ~1u; return Value(); });
  auto move = [](Object* s, bool first) {
    Object* p = s->parent;
    if (!p) return;
    auto& k = p->children;
    auto it = std::find_if(k.begin(), k.end(), [s](const Ref<Object>& r) { return r.get() == s; });
    if (it == k.end()) return;
    Ref<Object> keep = *it;
    k.erase(it);
    if (first) k.insert(k.begin(), keep);
    else k.push_back(keep);
  };
  implement(vm, "gsModule", u"moveToFirst", [move](VM&, Object* s, int, const Value*) { move(s, true); return Value(); });
  implement(vm, "gsModule", u"moveToLast", [move](VM&, Object* s, int, const Value*) { move(s, false); return Value(); });
  implement(vm, "gsModule", u"tick", [](VM& v, Object* s, int, const Value* a) {
    s->tick(v, v.toFloat(a[0]));
    return Value();
  });
  // owner: the parent module (+0x44); writing it reparents through setParent (0x1009ff90 → slot 0xC0)
  if (ClassInfo* m = vm.findClass("gsModule")) {
    FieldInfo& f = m->fields[u"owner"];
    f.name = u"owner";
    f.type = 8;
    f.get = [](VM&, Object* o) { return o->parent ? Value::heap(VT::Object, o->parent) : Value::null(); };
    f.set = [](VM& v, Object* o, const Value& x) {
      Object* np = v.resolve(x).object();
      if (np && !v.isModule(np)) np = nullptr;
      Ref<Object> keep(o);
      if (np) np->addChild(keep);
      else if (o->parent) o->parent->removeChild(o);
    };
  }
}

}  // namespace regs::engine
