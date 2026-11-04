// gsGrid: map loading (map.grid terrain and network, docs/save-format/README.md §3) and the item templates that
// scripts register with loadGridObject. The simulation sections are read by engine/sim.cpp.
#include "engine/grid.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/scene3d.h"
#include "engine/sim.h"
#include "engine/sim_graph.h"
#include "engine/terrain.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::script;

namespace {

struct Reader {
  const std::vector<uint8_t>& d;
  uint32_t u32(size_t at) const {
    uint32_t v = 0;
    if (at + 4 <= d.size()) std::memcpy(&v, d.data() + at, 4);
    return v;
  }
  uint16_t u16(size_t at) const {
    uint16_t v = 0;
    if (at + 2 <= d.size()) std::memcpy(&v, d.data() + at, 2);
    return v;
  }
  template <typename F>
  void children(size_t from, size_t end, F fn) const {
    end = std::min(end, d.size());
    for (size_t at = from; at + 8 <= end;) {
      uint32_t tag = u32(at), size = u32(at + 4);
      size_t body = at + 8, stop = std::min(end, body + size);
      fn(tag, body, stop);
      at = body + size;
    }
  }
};

}  // namespace

std::vector<uint32_t> savedItemRefs(const GridData& g) {
  std::vector<uint32_t> out(g.items.size(), 0);
  uint32_t n = 0;
  for (size_t i = 0; i < g.items.size(); ++i)
    if (!g.items[i].removed) out[i] = ++n;
  return out;
}

bool parseGridFile(const std::vector<uint8_t>& d, GridData& g, std::string* error) {
  Reader r{d};
  if (d.size() < 8 + 24 || r.u32(0) != 0x07000000) {
    if (error) *error = "not a grid file";
    return false;
  }
  size_t root = 8, end = std::min(d.size(), size_t(8) + r.u32(4));
  g.width = int(r.u32(root));
  g.length = int(r.u32(root + 4));
  std::memcpy(&g.cellSize, d.data() + root + 8, 4);
  g.bucketShift = r.u32(root + 12);
  g.heightBase = r.u16(root + 16);
  size_t cells = size_t(std::max(0, g.width)) * size_t(std::max(0, g.length));
  r.children(root + 24, end, [&](uint32_t tag, size_t body, size_t stop) {
    if (tag == 0x07010000) {  // terrain: i16 height, u16 flags per cell, row-major
      size_t n = std::min(cells, (stop - body) / 4);
      g.height.resize(cells);
      g.flags.resize(cells);
      int16_t shift = int16_t(0x200 - int(g.heightBase));
      for (size_t i = 0; i < n; ++i) {
        g.height[i] = int16_t(int16_t(r.u16(body + i * 4)) + shift);
        g.flags[i] = r.u16(body + i * 4 + 2);
      }
    } else if (tag == 0x07020000) {  // network: type names, 16-byte items, names
      uint32_t typeCount = r.u32(body), itemCount = r.u32(body + 4);
      r.children(body + 8, stop, [&](uint32_t t2, size_t b2, size_t s2) {
        if (t2 == 0x07020100) {
          size_t p = b2;
          for (uint32_t i = 0; i < typeCount && p < s2; ++i) {
            std::string s;
            while (p < s2 && d[p]) s += char(d[p++]);
            ++p;
            g.types.push_back(s);
          }
        } else if (t2 == 0x07020210 || t2 == 0x07020211) {
          for (uint32_t i = 0; i < itemCount && b2 + size_t(i + 1) * 16 <= s2; ++i) {
            size_t p = b2 + size_t(i) * 16;
            GridData::Item it;
            uint16_t tw = r.u16(p);
            it.type = tw & 0x3FFF;
            it.orient = uint8_t(tw >> 14);
            it.x = int16_t(r.u16(p + 2));
            it.y = int16_t(r.u16(p + 4));
            it.z = int16_t(r.u16(p + 6));
            it.parent = r.u32(p + 8);
            uint32_t f = r.u32(p + 12);
            it.variation = f & 0xFF;
            it.flags = f & ~0xFFu;
            g.items.push_back(it);
          }
        } else if (t2 == 0x07020200) {
          LOG_WARN("grid: old 8-byte item format is not supported yet");
        } else if (t2 == 0x07020310) {  // UTF-8, every entry padded to 4 bytes
          size_t p = b2;
          for (uint32_t i = 0; i < itemCount && p < s2; ++i) {
            size_t start = p;
            std::string s;
            while (p < s2 && d[p]) s += char(d[p++]);
            ++p;
            p = start + (p - start + 3) / 4 * 4;
            g.names.push_back(s);
          }
        }
      });
    }
  });
  if (g.height.size() != cells) {
    if (error) *error = "terrain section missing";
    return false;
  }
  return true;
}

bool parseBmp(const std::vector<uint8_t>& d, int& width, int& height, std::vector<uint32_t>& argb) {
  Reader r{d};
  if (d.size() < 54 || d[0] != 'B' || d[1] != 'M') return false;
  uint32_t offset = r.u32(10), header = r.u32(14);
  int w = int(r.u32(18)), h = int(int32_t(r.u32(22)));
  int bpp = r.u16(28);
  if (r.u32(30) != 0 || w <= 0 || h == 0 || (bpp != 8 && bpp != 24 && bpp != 32)) return false;
  bool bottomUp = h > 0;
  h = std::abs(h);
  size_t stride = (size_t(w) * size_t(bpp) / 8 + 3) & ~size_t(3);
  if (offset + stride * size_t(h) > d.size()) return false;
  size_t palette = 14 + header;
  width = w;
  height = h;
  argb.assign(size_t(w) * size_t(h), 0xFF000000u);
  for (int y = 0; y < h; ++y) {
    const uint8_t* row = d.data() + offset + stride * size_t(bottomUp ? h - 1 - y : y);
    for (int x = 0; x < w; ++x) {
      const uint8_t* p = bpp == 8 ? d.data() + std::min(palette + size_t(row[x]) * 4, d.size() - 4) : row + size_t(x) * size_t(bpp / 8);
      argb[size_t(y) * size_t(w) + size_t(x)] = 0xFF000000u | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0];
    }
  }
  return true;
}

void registerGridNatives(VM& vm) {
  if (ClassInfo* c = vm.findClass("gsGrid")) c->factory = [](const ClassInfo* ci) { return Ref<Object>(new GridObj(ci)); };
  // loadGridObject(idName, objectFile, [fallbackFile, [scaleX, scaleZ]]) (0x100c2b20, docs/renderer/grid-items.md §1):
  // a gsGridObject template. Like gsObject::loadFromFile: the companion script (width, flags, $price, storeMesh, ...),
  // then the mesh, then onLoaded (shaders through getMaterial).
  implement(vm, "gsGrid", u"loadGridObject", [](VM& v, Object* s, int argc, const Value* a) {
    auto* g = dynamic_cast<GridObj*>(s);
    if (!g) return Value();
    std::string name = utf16ToUtf8(v.toStr(a[0]));
    if (name.size() > 31) LOG_WARN("Object ID too long: %s", name.c_str());
    if (g->templates.count(name)) LOG_WARN("Warning: Dublicate object ID: %s", name.c_str());
    Ref<Object> t = v.newObject(v.findClass("gsGridObject"), nullptr);
    auto* o = dynamic_cast<ObjectObj*>(t.get());
    std::u16string path = v.resolvePath(v.toStr(a[1]), s), meshPath = path;
    if (argc > 2) {
      // the diagonal "b" variant: the fallback's mesh turned by 45°, with the variant's own script if it has one
      std::u16string fallback = v.resolvePath(v.toStr(a[2]), s);
      if (!v.vfs().exists(utf16ToUtf8(path.substr(0, path.rfind(u'.')) + u".script"))) path = fallback;
      meshPath = fallback;
      if (o) o->loadYaw = 0.78539816f;
      if (o && argc > 4) {
        o->loadScale[0] = float(v.toFloat(a[3]));
        o->loadScale[2] = float(v.toFloat(a[4]));
      }
    }
    t->fieldValues[u"idName"] = Value::string(utf8ToUtf16(name));
    g->templates[name] = t;
    g->templateOrder.push_back(name);
    runCompanionScript(v, t.get(), path);
    if (o) {
      o->pendingPath = utf16ToUtf8(meshPath);
      o->data = loadObjectCached(v, o->pendingPath);
      markTemplateMeshLoaded(v, o);  // the locator paths, before onLoaded's setPathFlags
      if (o->data) v.fireEvent(o, u"onLoaded", {});
    }
    return Value::heap(VT::Object, t.get());
  });
  // setTextureVariation(n, material): the material that replaces the non-0x08000000 submesh materials of merged
  // items with record variation n (1..4)
  implement(vm, "gsGridObject", u"setTextureVariation", [](VM& v, Object* s, int, const Value* a) {
    int n = v.toInt(a[0]);
    if (n >= 1 && n <= 4) s->fieldValues[u"<variation" + utf8ToUtf16(std::to_string(n)) + u">"] = v.resolve(a[1]);
    return Value();
  });
  // loadMaterial(video, path): a material variant read from a .material file
  implementGlobal(vm, u"loadMaterial", [](VM& v, Object* self, int, const Value* a) {
    Ref<Object> m = v.newObject(v.findClass("gsMaterialVariant"), nullptr);
    std::u16string path = v.resolvePath(v.toStr(a[1]), self);
    m->fieldValues[u"<materialFile>"] = Value::string(path);
    return Value::heap(VT::Object, m.get());
  });
  // colormap(path): a BMP as a top-down RGB pixel table (L:0x080eecb0) for per-item tints
  if (ClassInfo* c = vm.findClass("gsObjectVariant")) {
    implementGlobal(vm, u"colormap", [c](VM& v, Object* self, int, const Value* a) {
      Ref<Object> o(new ColormapObj(c));
      auto* cm = static_cast<ColormapObj*>(o.get());
      std::string path = utf16ToUtf8(v.resolvePath(v.toStr(a[0]), self));
      std::vector<uint8_t> file;
      if (!v.vfs().read(path, file) || !parseBmp(file, cm->width, cm->height, cm->argb)) LOG_WARN("colormap %s: not a readable BMP", path.c_str());
      return Value::heap(VT::Object, o.get());
    });
  }
  implement(vm, "gsGrid", u"getGridObject", [](VM& v, Object* s, int, const Value* a) {
    auto* g = dynamic_cast<GridObj*>(s);
    if (!g) return Value::null();
    auto it = g->templates.find(utf16ToUtf8(v.toStr(a[0])));
    return it == g->templates.end() ? Value::null() : Value::heap(VT::Object, it->second.get());
  });
  // getYardMaterial(i) / getFieldMaterial(i): one material per index, created on first request
  for (const char16_t* m : {u"getYardMaterial", u"getFieldMaterial"}) {
    std::u16string key = std::u16string(u"<") + m + u">";
    implement(vm, "gsGrid", m, [key](VM& v, Object* s, int, const Value* a) {
      std::u16string k = key + utf8ToUtf16(std::to_string(v.toInt(a[0])));
      Value& slot = s->fieldValues[k];
      if (!slot.object()) slot = Value::heap(VT::Object, v.newObject(v.findClass("gsMaterialVariant"), nullptr).get());
      return slot;
    });
  }
  if (ClassInfo* c = vm.findClass("gsGrid"))
    for (const char16_t* n : {u"width", u"length"}) {
      FieldInfo& f = c->fields[n];
      f.name = n;
      f.readOnly = true;
      bool w = n[0] == u'w';
      f.get = [w](VM&, Object* o) {
        auto* g = dynamic_cast<GridObj*>(o);
        return Value::integer(g && g->data ? (w ? g->data->width : g->data->length) : 0);
      };
    }
  // getAreaY(x, z, diameter, itemWeight) (L:0x0813ff00): the highest terrain sample around (x, z), faded out with a
  // cosine towards the rim. Items do not raise it yet (docs/ui/game-load.md §6.5).
  implement(vm, "gsGrid", u"getAreaY", [](VM& v, Object* s, int, const Value* a) {
    auto* g = dynamic_cast<GridObj*>(s);
    if (!g || !g->data) return Value::real(0.0f);
    const GridData& d = *g->data;
    float sc = d.cellSize;
    int cx = int(std::lrint((v.toFloat(a[0]) + d.width * sc * 0.5f) / sc));
    int cz = int(std::lrint((v.toFloat(a[1]) + d.length * sc * 0.5f) / sc));
    int r = int(std::lrint(0.5f * v.toFloat(a[2]) / sc)), r2 = r * r;
    int x0 = std::max(cx - r, 0), x1 = std::min(cx + r, d.width - 1);
    int z0 = std::max(cz - r, 0), z1 = std::min(cz + r, d.length - 1);
    int h = 0;
    for (int j = z0; j <= z1; j += 4)
      for (int i = x0; i <= x1; i += 4) {
        int d2 = (i - cx) * (i - cx) + (j - cz) * (j - cz);
        if (d2 >= r2) continue;
        double w = std::sin(double(float(d2) / float(r2) * 3.1415927f + 1.5707964f)) * 0.5 + 0.5;
        h = std::max(h, int(std::lrint(d.height[size_t(j) * d.width + i] * w)));
      }
    return Value::real(float(h) * sc * 0.00390625f);
  });

  // ---- simulation stand-ins (milestone 5): counts are 0, lookups find nothing, lists are empty ----
  auto zero = [](VM&, Object*, int, const Value*) { return Value::integer(0); };
  auto none = [](VM&, Object*, int, const Value*) { return Value::null(); };
  for (const char16_t* m : {u"getHomeCount", u"getWorkplaceCount", u"getWalkCount", u"getDriveCount", u"getTravelCount",
                            u"getCanTravelCount", u"getPriceTooHighCount", u"getUnemployment"})
    implement(vm, "gsGridCitizens", m, zero);
  implement(vm, "gsGridCitizens", u"findCitizen", none);
  implement(vm, "gsGridCitizens", u"getRandomCitizen", none);
  implement(vm, "gsGrid", u"getAreaType", zero);  // ambience zone around the camera (0 = default)

  implement(vm, "gsGrid", u"loadFromFile", [](VM& v, Object* s, int, const Value* a) {
    auto* g = dynamic_cast<GridObj*>(s);
    if (!g) return Value::boolean(false);
    std::string path = utf16ToUtf8(v.resolvePath(v.toStr(a[0]), s));
    std::vector<uint8_t> file;
    auto data = std::make_shared<GridData>();
    std::string err;
    if (!v.vfs().read(path, file) || !parseGridFile(file, *data, &err)) {
      LOG_WARN("grid %s: %s", path.c_str(), err.empty() ? "not found" : err.c_str());
      return Value::boolean(false);
    }
    prepareTerrainCells(*data);
    size_t missing = 0;
    for (const auto& t : data->types) missing += g->templates.count(t) ? 0 : 1;
    LOG_INFO("grid %s: %dx%d cells, %zu items of %zu types (%zu without a template)", path.c_str(), data->width,
             data->length, data->items.size(), data->types.size(), missing);
    g->data = data;
    g->sourceFile = std::make_shared<const std::vector<uint8_t>>(file);
    ++g->generation;
    loadSimulation(v, g, file);
    return Value::boolean(true);
  });
}

void GridObj::tick(VM& vm, float dt) {
  tickSimulation(vm, this, dt);
  Object::tick(vm, dt);
}

}  // namespace regs::engine
