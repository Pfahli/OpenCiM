// gsGraphCtrl (docs/ui/graphs.md): up to ten series of samples in ring buffers, drawn as line strips over a grid with
// value labels. The original rebuilds its GUI items whenever the data, rect or visibility change (updateLines); the
// port computes the same geometry from the same state when it draws the control.
#include "engine/ui_graphs.h"

#include <algorithm>
#include <cmath>

#include "engine/api.h"

namespace regs::engine {

using namespace regs::script;

namespace {

// The ctor's series colours (L:0x081d1a20 from the DefColors table, L:0x080d0df0): white, red, blue, yellow, lime,
// fuchsia, aqua, orange, then white and red again
constexpr uint32_t kDefColors[GraphObj::kSeries] = {0xFFFFFFFFu, 0xFFFF0000u, 0xFF0000FFu, 0xFFFFFF00u, 0xFF00FF00u,
                                                    0xFFFF00FFu, 0xFF00FFFFu, 0xFFFF8000u, 0xFFFFFFFFu, 0xFFFF0000u};

GraphObj* graph(Object* o) { return static_cast<GraphObj*>(o); }

// _ftol (FUN_101210d8): the Windows build truncates where the Linux build rounds
int ftol(float f) { return int(f); }

void field(ClassInfo* c, const char16_t* name, int type, std::function<Value(GraphObj*)> get,
           std::function<void(VM&, GraphObj*, const Value&)> set) {
  FieldInfo& f = c->fields[name];
  f.name = name;
  f.type = type;
  f.readOnly = false;
  f.get = [get](VM&, Object* o) { return get(graph(o)); };
  f.set = [set](VM& vm, Object* o, const Value& v) { set(vm, graph(o), v); };
}

}  // namespace

GraphObj::GraphObj(const ClassInfo* ci) : ControlObj(ci) {
  for (int i = 0; i < kSeries; ++i) series[i].color = kDefColors[i];
}

void GraphObj::update(VM& vm, int target) {
  // update(int) (L:0x081d1cd0, W:0x100ab730): step the ring index to 'target'; every step stores each series' argument
  // (setArg: value, reference or macro) and widens minValue/maxValue to it
  int seg = std::max(1, segmentCount);
  int i = current;
  if (i != target) {
    do {
      i = (i + 1) % seg;
      for (int s = 0; s < kSeries; ++s) {
        if (size_t(s) >= args.size() || args[size_t(s)].isNone()) continue;
        Series& ser = series[s];
        if (ser.data.size() != size_t(seg)) continue;  // reset() dropped the buffer (the original would crash)
        float v = vm.toFloat(vm.resolve(args[size_t(s)]));
        ser.data[size_t(i)] = v;
        if (maxValue < v) maxValue = v;
        if (v < minValue) minValue = v;
      }
    } while (i != target);
  }
  current = target;
}

void GraphObj::tick(VM& vm, float dt) {
  // move (L:0x081d2160, W:0x100ab810): with automaticUpdate, 'rate' samples per second of the control's clock
  if (automaticUpdate) {
    clock += rate * dt;
    int idx = ftol(clock) % std::max(1, segmentCount);
    if (idx != current) update(vm, idx);
  }
  ControlObj::tick(vm, dt);
}

void graphGeometry(VM& vm, GraphObj* g, const std::function<float(const std::u16string&)>& textWidth, GraphGeometry& out) {
  const float b = g->borderWidth, L = g->desktopLeft(), T = g->desktopTop(), W = g->width, H = g->height;
  const float mn = g->minValue, mx = g->maxValue;
  const int seg = std::max(1, g->segmentCount), view = g->viewOffset, n = seg - view;
  // value → y: maxValue at 10 % below the top, minValue at 10 % above the bottom (both inset by the border)
  const float py = mn < mx ? (H * 0.8f - (b + b)) / (mx - mn) : 0.0f;
  const float base = mn * py + ((H * 0.9f + T) - b);
  if (n < 2) return;
  const float dx = (W - (b + b)) / float(n - 1);

  // updateLine (L:0x081d0c40, W:0x100ac4d0): ring positions view..seg-1, the newest sample at the right edge; the
  // series colour as is (no visibility)
  for (const GraphObj::Series& s : g->series) {
    if (!s.line || s.data.size() != size_t(seg)) continue;
    std::vector<GraphGeometry::Vertex> strip;
    strip.reserve(size_t(n));
    for (int i = view; i < seg; ++i)
      strip.push_back({float(i - view) * dx + L + b, base - py * s.data[size_t((g->current + i + 1) % seg)], s.color});
    out.strips.push_back(std::move(strip));
  }

  // grid (updateLines, L:0x081d2f30, W:0x100ae7c0): text colour × 0.4 for every verticalLines-th / horizontalLines-th
  // line, × 0.15 (faded further when dense) for the others
  const int vl = g->verticalLines, hl = g->horizontalLines;
  if (vl != 0 || hl != 0) {
    const float vis = g->visibility;
    const uint32_t major = mulA(g->textColor, 0.4f * vis);
    const int nv = vl != 0 ? n : 0;
    if (nv != 0) {
      float fade = std::min(1.0f, dx * (dx < 1.0f ? 100.0f : 0.1f));
      uint32_t minor = mulA(g->textColor, 0.15f * vis * fade);
      for (int i = 0; i < nv; ++i) {
        float x = float(i) * dx + L + b;
        uint32_t c = (nv - 1 - i) % vl == 0 ? major : minor;
        out.grid.push_back({x, T + b, c});
        out.grid.push_back({x, (T + H) - b, c});
      }
    }
    if (hl != 0 && mn < mx) {
      // step: the smallest power of horizontalLines (from hl^-6, below 1000) giving at least hl/2 steps over the range
      int h2 = hl * hl;
      float scale = 1.0f / float(h2 * h2 * h2);
      const float range = mx - mn;
      int k;
      for (;;) {
        k = ftol(scale * range);
        if (hl <= k * 2) break;
        scale *= float(hl);
        if (!(scale < 1000.0f)) {
          k = ftol(scale * range);
          break;
        }
      }
      const int nh = (k * 6 + 4) / 5 + 2;
      float fade = 1.0f - float(nh) / float(hl * nh);
      uint32_t minor = mulA(g->textColor, 0.15f * vis * std::max(0.0f, fade));
      const int first = ftol(scale * mn - 0.5f) - nh / 10;
      for (int j = 0; j < nh; ++j) {
        int step = first + j;
        float y = base - (float(step) / scale) * py;
        uint32_t c = step % hl == 0 ? major : minor;
        out.grid.push_back({L + b, y, c});
        out.grid.push_back({(L + W) - b, y, c});
      }
    }
  }

  // labels (updateTexts, L:0x081d2220, W:0x100adb50): only with a textFormat; zero line, maximum, minimum (each hidden
  // when outside the range or squeezed against zero) at the left edge, the newest value of series 0 at the right
  if (g->textFormat.empty()) return;
  const float sy = g->sizeY;
  auto yAt = [&](float v) {
    float y = (base - v * py) - sy * 0.5f;
    return sy >= 1.0f ? float(ftol(y)) : y;
  };
  auto fmt = [&](Value v) { return formatPattern(vm, g->textFormat, &v, 1); };
  if (!(0.0f < mn || mx < 0.0f)) out.labels.push_back({fmt(Value::integer(0)), L, yAt(0.0f)});
  if (!(mx <= mn || (mn < 0.0f && mx / -mn <= 0.2f))) out.labels.push_back({fmt(Value::real(mx)), L, yAt(mx)});
  if (!(mx <= mn || (0.0f < mx && mn / -mx <= 0.2f))) out.labels.push_back({fmt(Value::real(mn)), L, yAt(mn)});
  const GraphObj::Series& s0 = g->series[0];
  if (s0.data.size() == size_t(seg)) {
    float v = s0.data[size_t(g->current)];
    std::u16string t = fmt(Value::real(v));
    out.labels.push_back({t, (L + W) - textWidth(t), yAt(v)});
  }
}

void registerGraphNatives(VM& vm) {
  ClassInfo* c = vm.findClass("gsGraphCtrl");
  if (!c) return;
  c->factory = [](const ClassInfo* ci) { return Ref<Object>(new GraphObj(ci)); };
  // fields (initializeClassData, L:0x081d0820, W:0x100ad460)
  field(c, u"automaticUpdate", 2, [](GraphObj* g) { return Value::boolean(g->automaticUpdate); },
        [](VM& v, GraphObj* g, const Value& x) { g->automaticUpdate = v.truthy(x); });
  field(c, u"segmentCount", 3, [](GraphObj* g) { return Value::integer(g->segmentCount); },
        [](VM& v, GraphObj* g, const Value& x) {
          // a plain store in the original (scripts set it before the first setArg); the port keeps existing buffers
          // and the ring index in range
          g->segmentCount = std::max(1, v.toInt(x));
          for (GraphObj::Series& s : g->series)
            if (!s.data.empty()) s.data.resize(size_t(g->segmentCount), 0.0f);
          g->current %= g->segmentCount;
        });
  field(c, u"viewOffset", 3, [](GraphObj* g) { return Value::integer(g->viewOffset); },
        [](VM& v, GraphObj* g, const Value& x) {
          // write_viewOffset (L:0x081d0ac0, W:0x100ac480): clamped to 0..segmentCount−2
          int o = v.toInt(x), hi = g->segmentCount - 2;
          g->viewOffset = o < hi ? std::max(0, o) : hi;
        });
  field(c, u"minValue", 4, [](GraphObj* g) { return Value::real(g->minValue); },
        [](VM& v, GraphObj* g, const Value& x) { g->minValue = v.toFloat(x); });
  field(c, u"maxValue", 4, [](GraphObj* g) { return Value::real(g->maxValue); },
        [](VM& v, GraphObj* g, const Value& x) { g->maxValue = v.toFloat(x); });
  field(c, u"textFormat", 6, [](GraphObj* g) { return Value::string(g->textFormat); },
        [](VM& v, GraphObj* g, const Value& x) { g->textFormat = v.toStr(x); });
  field(c, u"verticalLines", 3, [](GraphObj* g) { return Value::integer(g->verticalLines); },
        [](VM& v, GraphObj* g, const Value& x) { g->verticalLines = v.toInt(x); });
  field(c, u"horizontalLines", 3, [](GraphObj* g) { return Value::integer(g->horizontalLines); },
        [](VM& v, GraphObj* g, const Value& x) { g->horizontalLines = v.toInt(x); });

  // setArg(i, v) (L:0x081d0650, W:0x100abca0): a series' first argument allocates its zeroed ring buffer and its line
  implement(vm, "gsGraphCtrl", u"setArg", [](VM& v, Object* s, int, const Value* a) {
    GraphObj* g = graph(s);
    int i = v.toInt(a[0]);
    if (i < 0) return Value();
    if (i < GraphObj::kSeries) {
      GraphObj::Series& ser = g->series[i];
      if (ser.data.empty()) ser.data.assign(size_t(std::max(1, g->segmentCount)), 0.0f);
      ser.line = true;
    }
    if (size_t(i) >= g->args.size()) g->args.resize(size_t(i) + 1);
    g->args[size_t(i)] = a[1];  // gsControl::setArg: kept as given ('@' references and macros stay live)
    return Value();
  });
  // setColor(i, colour) / getColor(i) (L:0x081cf6a0 / 0x081d07a0, W:0x100ab490 / 0x100ac430)
  implement(vm, "gsGraphCtrl", u"setColor", [](VM& v, Object* s, int, const Value* a) {
    int i = v.toInt(a[0]);
    if (i >= 0 && i < GraphObj::kSeries) graph(s)->series[i].color = uint32_t(v.toInt(a[1]));
    return Value();
  });
  implement(vm, "gsGraphCtrl", u"getColor", [](VM& v, Object* s, int, const Value* a) {
    int i = v.toInt(a[0]);
    return Value::integer(i >= 0 && i < GraphObj::kSeries ? int32_t(graph(s)->series[i].color) : 0);
  });
  // update() (L:0x081d21f0, W:0x100ac460): one sample of every series' argument
  implement(vm, "gsGraphCtrl", u"update", [](VM& v, Object* s, int, const Value*) {
    GraphObj* g = graph(s);
    g->update(v, (g->current + 1) % std::max(1, g->segmentCount));
    return Value();
  });
  // reset() (L:0x081cedd0, W:0x100ab4d0): drops every buffer and line; ring index, range and clock back to 0. The
  // arguments and colours stay.
  implement(vm, "gsGraphCtrl", u"reset", [](VM&, Object* s, int, const Value*) {
    GraphObj* g = graph(s);
    for (GraphObj::Series& ser : g->series) {
      ser.data.clear();
      ser.line = false;
    }
    g->current = 0;
    g->maxValue = g->minValue = 0;
    g->clock = 0;
    return Value();
  });
}

}  // namespace regs::engine
