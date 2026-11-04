// Engine object classes with their own state and per-frame tick: gsModule flag bits, gsClock, gsTimerClock and
// gsStateMachine. Behaviour follows the decompiled engine (addresses cited per function).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "script/compiler.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::script;

namespace {

Value plainOf(const Value& v) { return v.ref() ? v.ref()->get() : v; }

// ---------------------------------------------------------------------------------------------------------------
// gsClock (0x10073b30 ctor, 0x10073f80 tick, 0x10073c40 calendar + events)

class ClockObj : public Object {
 public:
  using Object::Object;
  float speed = 1.0f;
  float acc = 0;      // seconds not yet moved into 'time'
  int32_t date = 0;   // days since 1 Jan of year 0
  int32_t time = 0;   // ms since midnight
  int32_t ms = 0, sec = 0, min = 0, hour = 0, day = 0, month = 0, year = 0;
  bool paused = false;

  void tick(VM& vm, float dt) override {
    if (!paused) acc += dt * speed;
    int64_t whole = int64_t(double(acc) * 1000.0);
    time += int32_t(whole);
    acc -= float(whole) * 0.001f;
    while (time > 86399999) { time -= 86400000; ++date; }
    while (time < 0) { time += 86400000; --date; }
    recompute(vm, true);
    vm.tickChildren(this, dt);
  }

  // decode date/time; fires onHour, onDay, onMonth, onYear when the respective parts changed
  void recompute(VM& vm, bool fire) {
    int32_t d = date;
    int32_t q = d / 146097;
    if (d < 0 && d % 146097) --q;
    d -= q * 146097;
    int32_t c = 0, y4 = 0, yr = 0;
    bool leap = false;
    if (d < 36525) {
      y4 = d / 1461;
      d -= y4 * 1461;
    } else {
      c = (d - 36525) / 36524 + 1;
      d = d - 36525 - (c - 1) * 36524;
      if (d < 1460) {
        yr = d / 365;
        d %= 365;
        goto months;
      }
      d -= 1460;
      y4 = d / 1461 + 1;
      d -= (y4 - 1) * 1461;
    }
    if (d < 366) {
      leap = true;
    } else {
      yr = (d - 366) / 365 + 1;
      d = d - 366 - (yr - 1) * 365;
    }
  months:
    int32_t y = yr + ((c + q * 4) * 25 + y4) * 4;
    if (!leap && d > 58) ++d;  // skip 29 Feb in common years
    static const int start[13] = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366};
    int m = 1;
    while (m < 12 && d >= start[m]) ++m;
    int dd = d - start[m - 1] + 1;
    bool yC = y != year, mC = m != month, dC = dd != day;
    int h = time / 3600000;
    bool hC = h != hour;
    year = y;
    month = m;
    day = dd;
    hour = h;
    int r = time % 3600000;
    min = r / 60000;
    r %= 60000;
    sec = r / 1000;
    ms = r % 1000;
    if (!fire) return;
    if (yC || mC || dC || hC) {
      vm.fireEvent(this, u"onHour", {});
      if (yC || mC || dC) {
        vm.fireEvent(this, u"onDay", {});
        if (yC || mC) {
          vm.fireEvent(this, u"onMonth", {});
          if (yC) vm.fireEvent(this, u"onYear", {});
        }
      }
    }
  }
};

// gsTimerClock: counts down and fires onTimeout once, then destroys itself (0x10073ae0)
class TimerObj : public Object {
 public:
  using Object::Object;
  float duration = 0, left = 0;

  void tick(VM& vm, float dt) override {
    left -= dt;
    if (left <= 0.0f && !destroyed) {
      Ref<Object> keep(this);
      vm.fireEvent(this, u"onTimeout", {});
      vm.destroy(this);
      return;
    }
    vm.tickChildren(this, dt);
  }
};

// ---------------------------------------------------------------------------------------------------------------
// gsStateMachine (.states, docs/REPORT.md §4.4; runtime 0x1009e900 tick, 0x1009ce70 process, 0x1009bd10 transition,
// 0x1009d060 enter, 0x1009d780 initial)

struct SmState {
  std::u16string name;
  uint32_t flags = 0;      // 1 choice, 2 timed, 4 initial, 8 final
  float seconds = 0;
  SmState* returnTo = nullptr;
};

struct SmTransition {
  std::u16string action;
  uint32_t flags = 0;      // 1: return to the previous state
  uint32_t branch = 0;
  SmState* from = nullptr;
  SmState* to = nullptr;
  std::u16string guardSrc, actionSrc;
  FragmentInfo guard, act;
  bool hasGuard = false, hasAction = false;
};

class StateMachineObj : public Object {
 public:
  using Object::Object;
  std::vector<std::unique_ptr<SmState>> states;  // file order
  std::vector<SmTransition> transitions;
  std::deque<std::u16string> queue;
  SmState* current = nullptr;
  SmState* prev = nullptr;
  float stateTime = 0;

  bool parse(const std::vector<uint8_t>& b, std::string* err);
  void enterInitial(VM& vm);
  bool process(VM& vm, SmState* state, const std::u16string& action);
  void transition(VM& vm, SmTransition& t);
  void enter(VM& vm, SmState* s);

  void tick(VM& vm, float dt) override {
    if (current && (current->flags & 2) && stateTime < current->seconds) {
      stateTime += dt;
      if (current->seconds <= stateTime) queue.push_back(u"timeout");
    }
    // the engine's loop compares a growing counter with the shrinking queue: about half the queue per tick
    size_t i = 0;
    if (!queue.empty()) {
      do {
        std::u16string a = queue.front();
        process(vm, current, a);
        if (!queue.empty()) queue.pop_front();
        ++i;
      } while (i < queue.size());
    }
    vm.tickChildren(this, dt);
  }
};

uint32_t rd32(const std::vector<uint8_t>& b, size_t at) { return uint32_t(b[at]) | b[at + 1] << 8 | b[at + 2] << 16 | uint32_t(b[at + 3]) << 24; }
uint16_t rd16(const std::vector<uint8_t>& b, size_t at) { return uint16_t(b[at] | b[at + 1] << 8); }
std::u16string latin(const std::vector<uint8_t>& b, size_t at, size_t max) {
  std::u16string s;
  for (size_t i = 0; i < max && at + i < b.size() && b[at + i]; ++i) s += cp1252ToUtf16(b[at + i]);
  return s;
}

bool StateMachineObj::parse(const std::vector<uint8_t>& b, std::string* err) {
  auto chunks = [&](size_t start, size_t end, auto&& fn) {
    for (size_t pos = start; pos + 8 <= end;) {
      uint32_t tag = rd32(b, pos), size = rd32(b, pos + 4);
      if (pos + 8 + size > end) break;
      fn(tag, pos + 8, size_t(size));
      pos += 8 + size;
    }
  };
  if (b.size() < 8 || rd32(b, 0) != 0x05000000) {
    if (err) *err = "not a state machine";
    return false;
  }
  std::vector<SmState*> byIndex;
  struct RawT { size_t body, size; };
  std::vector<RawT> raw;
  chunks(8, 8 + rd32(b, 4), [&](uint32_t, size_t body, size_t size) {
    chunks(body, body + size, [&](uint32_t tag, size_t c, size_t csize) {
      if (tag == 0x05010100 && csize >= 44) {
        std::u16string name = latin(b, c, 32);
        uint32_t flags = rd32(b, c + 32);
        float secs;
        uint32_t p = rd32(b, c + 36);
        std::memcpy(&secs, &p, 4);
        SmState* s = nullptr;
        if (!name.empty())  // a repeated name merges into the first state (0x1009f110)
          for (auto& e : states)
            if (e->name == name) s = e.get();
        if (s) {
          s->flags |= flags;
          if (flags & 2) s->seconds = secs;
        } else {
          states.push_back(std::make_unique<SmState>());
          s = states.back().get();
          s->name = name;
          s->flags = flags;
          s->seconds = (flags & 2) ? secs : 0;
        }
        byIndex.push_back(s);
      } else if (tag == 0x05020100 && csize >= 48) {
        raw.push_back({c, csize});
      }
    });
  });
  for (auto& r : raw) {
    SmTransition t;
    t.action = latin(b, r.body, 32);
    t.flags = rd32(b, r.body + 32);
    t.branch = rd32(b, r.body + 36);
    uint16_t from = rd16(b, r.body + 40), to = rd16(b, r.body + 42);
    t.from = from < byIndex.size() ? byIndex[from] : nullptr;
    t.to = to < byIndex.size() ? byIndex[to] : nullptr;
    chunks(r.body + 48, r.body + r.size, [&](uint32_t tag, size_t c, size_t csize) {
      std::u16string src = latin(b, c, csize);
      if (tag == 0x05020101) { t.guardSrc = src; t.hasGuard = true; }
      if (tag == 0x05020102) { t.actionSrc = src; t.hasAction = true; }
    });
    transitions.push_back(std::move(t));
  }
  // lookup order: (from, action, branch) like the engine's multimap key; equal keys keep file order
  std::stable_sort(transitions.begin(), transitions.end(), [](const SmTransition& a, const SmTransition& b) {
    return a.branch < b.branch;
  });
  return true;
}

void StateMachineObj::enterInitial(VM& vm) {
  if (current) {
    Value key = Value::string(current->name);
    vm.fireEvent(this, u"exit", {}, &key);
    prev = current;
    current = nullptr;
  }
  prev = nullptr;
  std::vector<SmState*> sorted;
  for (auto& s : states) sorted.push_back(s.get());
  std::stable_sort(sorted.begin(), sorted.end(), [](SmState* a, SmState* b) { return a->name < b->name; });
  for (SmState* s : sorted)
    if (s->flags & 4) {
      enter(vm, s);
      return;
    }
}

bool StateMachineObj::process(VM& vm, SmState* state, const std::u16string& action) {
  for (auto& t : transitions) {
    if (t.from != state || t.action != action) continue;
    if (!t.hasGuard) {
      transition(vm, t);
      return true;
    }
    Value r = vm.runFragment(this, t.guard.entry, t.guard.nlocals, true);
    bool ok = false;
    try {
      ok = !plainOf(r).isNone() && vm.truthy(r);
    } catch (const ScriptError&) {
      ok = false;
    }
    if (ok) {
      transition(vm, t);
      return true;
    }
  }
  return false;
}

void StateMachineObj::transition(VM& vm, SmTransition& t) {
  if (current) {
    Value key = Value::string(current->name);
    vm.fireEvent(this, u"exit", {}, &key);
    prev = current;
    current = nullptr;
  }
  if (t.hasAction) vm.runFragment(this, t.act.entry, t.act.nlocals, false);
  SmState* target = t.to;
  if (t.flags & 1) {  // return to the state that entered the previous one
    if (!prev) return;
    target = prev->returnTo;
  }
  enter(vm, target);
}

void StateMachineObj::enter(VM& vm, SmState* s) {
  auto clearChain = [&] {
    for (SmState* p = prev; p;) {
      SmState* n = p->returnTo;
      p->returnTo = nullptr;
      p = n;
    }
  };
  if (!s) {
    clearChain();
    current = nullptr;
    return;
  }
  if ((s->flags & 1) && process(vm, s, u"")) return;  // choice: the first true branch moves on
  if (!(s->flags & 8)) {
    if (!s->returnTo) {
      s->returnTo = prev;
    } else {
      for (SmState* p = prev; p && p != s;) {
        SmState* n = p->returnTo;
        p->returnTo = nullptr;
        p = n;
      }
    }
    current = s;
    Value key = Value::string(s->name);
    vm.fireEvent(this, u"enter", {}, &key);
    stateTime = 0;
    return;
  }
  clearChain();  // final state: the machine ends
  current = nullptr;
  vm.fireEvent(this, u"onEnd", {});
}

Ref<Object> loadStateMachine(VM& vm, const std::u16string& rawPath, Object* owner) {
  const ClassInfo* cls = vm.findClass("gsStateMachine");
  Ref<Object> o = vm.newObject(cls, vm.isModule(owner) ? owner : nullptr);
  auto* sm = static_cast<StateMachineObj*>(o.get());
  std::u16string path = vm.resolvePath(rawPath, owner);
  std::vector<uint8_t> data;
  std::string err;
  if (!vm.vfs().read(utf16ToUtf8(path), data)) {
    vm.error("File \"" + utf16ToUtf8(path) + "\" not found!");
  }
  if (!sm->parse(data, &err)) LOG_WARN("%s: %s", utf16ToUtf8(path).c_str(), err.c_str());
  // the companion script (<path>.script) is compiled into the machine; guards and actions compile into the same code
  std::u16string script = path.substr(0, path.rfind(u'.')) + u".script";
  LoadOptions opt;
  opt.into = o;
  opt.beforeLink = [sm](Code& code, std::string* e) {
    for (auto& t : sm->transitions) {
      if (t.hasGuard && !compileFragment(code, t.guardSrc, true, t.guard, e)) return false;
      if (t.hasAction && !compileFragment(code, t.actionSrc, false, t.act, e)) return false;
    }
    return true;
  };
  vm.load(u"/" + script, opt);
  sm->scriptPath = path;
  if (!sm->current) sm->enterInitial(vm);
  return o;
}

}  // namespace

// Read-only var fields that hold a sub-object the engine creates in the owner's constructor (gsSun.light,
// gsSky.material, gsGrid.*Material, gsObject.skeleton, ...). The port creates it on first access.
void subObjectField(VM& vm, const char* cls, const char16_t* field, const char* subClass) {
  ClassInfo* c = vm.findClass(cls);
  if (!c) return;
  auto it = c->fields.find(field);
  if (it == c->fields.end()) return;
  std::u16string name = field;
  std::string sub = subClass;
  it->second.get = [name, sub](VM& v, Object* o) {
    auto f = o->fieldValues.find(name);
    if (f != o->fieldValues.end() && f->second.object()) return f->second;
    Ref<Object> s = v.newObject(v.findClass(sub), nullptr);
    if (v.isModule(s.get()) && v.isModule(o)) o->addChild(s);  // e.g. the sun's light lives under the sun
    Value val = Value::heap(VT::Object, s.get());
    o->fieldValues[name] = val;
    return val;
  };
}

void registerObjectNatives(VM& vm) {
  // ---- sub-objects created by their owner ----
  subObjectField(vm, "gsSun", u"light", "gsLight");
  subObjectField(vm, "gsShadowEffect_OGL", u"light", "gsLight");
  subObjectField(vm, "gsSky", u"material", "gsMaterialVariant");
  subObjectField(vm, "gsBoatContrain", u"material", "gsMaterialVariant");
  subObjectField(vm, "gsObject", u"skeleton", "gsSkeleton");
  subObjectField(vm, "gsWindow", u"video", "gsVideo_OGL");
  for (const char16_t* m : {u"material", u"earthMaterial", u"baseMaterial", u"edgeMaterial", u"positiveMaterial", u"negativeMaterial",
                            u"waterMaterial", u"waterEdgeMaterial", u"dataMaterial", u"reservedMaterial"})
    subObjectField(vm, "gsGrid", m, "gsMaterialVariant");
  subObjectField(vm, "gsGrid", u"graph", "gsGridGraph");
  // sounds are silent for now; clone() still has to give the ambience script a new sound object
  implement(vm, "gsSound", u"clone", [](VM& v, Object* s, int, const Value*) {
    Ref<Object> c = v.newObject(s->cls, s->parent);
    c->fieldValues = s->fieldValues;
    return Value::heap(VT::Object, c.get());
  });
  subObjectField(vm, "gsGrid", u"vehicles", "gsGridVehicles");
  subObjectField(vm, "gsGrid", u"characters", "gsGridCharacters");
  subObjectField(vm, "gsGrid", u"citizens", "gsGridCitizens");
  subObjectField(vm, "gsGrid", u"lines", "gsTransportLines");
  for (const char16_t* m : {u"pathMaterial", u"tempPathMaterial", u"extraPathMaterial"})
    subObjectField(vm, "gsTransportLine", m, "gsMaterialVariant");

  // ---- gsModule bit fields (+0x6c): enabled 1, visible 2, audible 4 ----
  if (ClassInfo* m = vm.findClass("gsModule")) {
    auto bit = [m](const char16_t* name, uint32_t mask) {
      auto it = m->fields.find(name);
      if (it == m->fields.end()) return;
      it->second.get = [mask](VM&, Object* o) { return Value::boolean(o->flags & mask); };
      it->second.set = [mask](VM&, Object* o, const Value& v) {
        if (plainOf(v).toBool()) o->flags |= mask;
        else o->flags &= ~mask;
      };
    };
    bit(u"enabled", 1);
    bit(u"visible", 2);
    bit(u"audible", 4);
  }

  // ---- clocks ----
  if (ClassInfo* c = vm.findClass("gsClock")) {
    c->factory = [](const ClassInfo* ci) { return Ref<Object>(new ClockObj(ci)); };
    auto field = [c](const char16_t* name, std::function<Value(ClockObj*)> get, std::function<void(ClockObj*, const Value&)> set) {
      FieldInfo& f = c->fields[name];
      f.name = name;
      f.get = [get](VM&, Object* o) { return get(static_cast<ClockObj*>(o)); };
      if (set) f.set = [set](VM&, Object* o, const Value& v) { set(static_cast<ClockObj*>(o), plainOf(v)); };
      else f.readOnly = true;
    };
    field(u"speed", [](ClockObj* k) { return Value::real(k->speed); }, [](ClockObj* k, const Value& v) { k->speed = v.toFloat(); });
    field(u"date", [](ClockObj* k) { return Value::integer(k->date); }, [](ClockObj* k, const Value& v) { k->date = v.toInt(); });
    field(u"time", [](ClockObj* k) { return Value::integer(k->time); }, [](ClockObj* k, const Value& v) { k->time = v.toInt(); });
    field(u"paused", [](ClockObj* k) { return Value::boolean(k->paused); }, [](ClockObj* k, const Value& v) { k->paused = v.toBool(); });
    field(u"mseconds", [](ClockObj* k) { return Value::integer(k->ms); }, nullptr);
    field(u"seconds", [](ClockObj* k) { return Value::integer(k->sec); }, nullptr);
    field(u"minutes", [](ClockObj* k) { return Value::integer(k->min); }, nullptr);
    field(u"hours", [](ClockObj* k) { return Value::integer(k->hour); }, nullptr);
    field(u"days", [](ClockObj* k) { return Value::integer(k->day); }, nullptr);
    field(u"months", [](ClockObj* k) { return Value::integer(k->month); }, nullptr);
    field(u"years", [](ClockObj* k) { return Value::integer(k->year); }, nullptr);
    // writing date or time decodes the calendar at once, without events (getSaveGamesList sets date and reads days)
    for (const char16_t* n : {u"date", u"time"}) {
      FieldInfo& f = c->fields[n];
      f.set = [set = f.set](VM& vm, Object* o, const Value& v) {
        set(vm, o, v);
        static_cast<ClockObj*>(o)->recompute(vm, false);
      };
    }
  }
  // clock([time], [date]) / clock(h, m, s, d, mo, y): a gsClock under MAIN (0x10074280)
  implementGlobal(vm, u"clock", [](VM& v, Object*, int argc, const Value* a) {
    Value* main = v.findGlobal(u"MAIN");
    Ref<Object> o = v.newObject(v.findClass("gsClock"), main ? main->object() : nullptr);
    auto* k = static_cast<ClockObj*>(o.get());
    if (argc == 1) k->time = plainOf(a[0]).toInt();
    if (argc == 2) { k->time = plainOf(a[0]).toInt(); k->date = plainOf(a[1]).toInt(); }
    if (argc == 6) {
      k->time = ((plainOf(a[0]).toInt() * 60 + plainOf(a[1]).toInt()) * 60 + plainOf(a[2]).toInt()) * 1000;
      Value* mk = v.findGlobal(u"mkdate");
      if (mk) k->date = v.call(*mk, {a[3], a[4], a[5]}).toInt();
    } else if (argc != 0 && argc != 1 && argc != 2) {
      v.error(kErrArgCount, "Wrong number of arguments (" + std::to_string(argc) + ")!");
    }
    k->recompute(v, false);  // years, months, ... readable at once (sandbox page, loading screen)
    return Value::heap(VT::Object, o.get());
  });
  if (ClassInfo* c = vm.findClass("gsTimerClock")) c->factory = [](const ClassInfo* ci) { return Ref<Object>(new TimerObj(ci)); };
  implementGlobal(vm, u"timer", [](VM& v, Object*, int, const Value* a) {
    Value* main = v.findGlobal(u"MAIN");
    Ref<Object> o = v.newObject(v.findClass("gsTimerClock"), main ? main->object() : nullptr);
    auto* t = static_cast<TimerObj*>(o.get());
    t->duration = t->left = plainOf(a[0]).toFloat();
    return Value::heap(VT::Object, o.get());
  });
  implement(vm, "gsTimerClock", u"cancel", [](VM& v, Object* self, int, const Value*) {
    v.destroy(self);
    return Value();
  });
  implement(vm, "gsTimerClock", u"restart", [](VM&, Object* self, int, const Value*) {
    auto* t = static_cast<TimerObj*>(self);
    t->left = t->duration;
    return Value();
  });

  // ---- state machines ----
  if (ClassInfo* c = vm.findClass("gsStateMachine")) c->factory = [](const ClassInfo* ci) { return Ref<Object>(new StateMachineObj(ci)); };
  implementGlobal(vm, u"loadStateMachine", [](VM& v, Object*, int, const Value* a) {
    Ref<Object> o = loadStateMachine(v, v.toStr(a[0]), plainOf(a[1]).object());
    return Value::heap(VT::Object, o.get());
  });
  implement(vm, "gsStateMachine", u"perform", [](VM& v, Object* self, int, const Value* a) {
    std::u16string action = v.toStr(a[0]);
    for (auto& ch : action) ch = char16_t(ch & 0xFF);  // narrowed to a std::string like the engine
    static_cast<StateMachineObj*>(self)->queue.push_back(action);
    return Value();
  });
  implement(vm, "gsStateMachine", u"reset", [](VM& v, Object* self, int, const Value*) {
    static_cast<StateMachineObj*>(self)->enterInitial(v);
    return Value();
  });
}

}  // namespace regs::engine

namespace regs::engine {

std::u16string currentStateName(script::Object* o) {
  auto* sm = dynamic_cast<StateMachineObj*>(o);
  if (!sm) return u"<not a state machine>";
  return sm->current ? sm->current->name : u"<none>";
}

}  // namespace regs::engine
