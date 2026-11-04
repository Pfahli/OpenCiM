#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace regs::script {

// Intrusive reference counting, like the engine's gsVariant addRef/release (+0x04/+0x08).
class HeapObj {
 public:
  virtual ~HeapObj() = default;
  void addRef() { ++refs_; }
  void release() {
    if (--refs_ == 0) delete this;
  }
  int refs() const { return refs_; }

 private:
  int refs_ = 0;
};

template <typename T>
class Ref {
 public:
  Ref() = default;
  Ref(T* p) : p_(p) { if (p_) p_->addRef(); }  // NOLINT: implicit by design
  Ref(const Ref& o) : p_(o.p_) { if (p_) p_->addRef(); }
  Ref(Ref&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
  template <typename U>
  Ref(const Ref<U>& o) : p_(o.get()) { if (p_) p_->addRef(); }  // NOLINT
  ~Ref() { if (p_) p_->release(); }
  Ref& operator=(Ref o) noexcept { std::swap(p_, o.p_); return *this; }
  T* get() const { return p_; }
  T* operator->() const { return p_; }
  T& operator*() const { return *p_; }
  explicit operator bool() const { return p_ != nullptr; }
  bool operator==(const Ref& o) const { return p_ == o.p_; }

 private:
  T* p_ = nullptr;
};

template <typename T, typename... A>
Ref<T> make(A&&... a) { return Ref<T>(new T(std::forward<A>(a)...)); }

// Operator-dispatch categories (variant vtable +0x18, docs/script-runtime/values.md §2.1).
enum class Cat : uint8_t {
  None = 0, Bool = 1, Int = 2, Float = 3, String = 4, Object = 5, Array = 6, Function = 7, Event = 8, Map = 9,
  Vector = 10, Quat = 11,
};

enum class VT : uint8_t {
  None,      // valueless ("unset")
  Null,
  Bool,
  Int,
  Float,
  String,
  Array,
  Map,
  Object,    // script/engine object
  Function,  // script function, native function or bound method
  Macro,
  Thread,
  Vector,
  Quat,
  Ref,       // live reference created by '@'
};

class Value;
class StrObj;
class ArrayObj;
class MapObj;
class Object;
class FuncObj;
class MacroObj;
class ThreadObj;
class RefObj;

class Value {
 public:
  Value() : t_(VT::None) { u_.p = nullptr; }
  static Value none() { return Value(); }
  static Value null() { Value v; v.t_ = VT::Null; return v; }
  static Value boolean(bool b) { Value v; v.t_ = VT::Bool; v.u_.b = b; return v; }
  static Value integer(int32_t i) { Value v; v.t_ = VT::Int; v.u_.i = i; return v; }
  static Value real(float f) { Value v; v.t_ = VT::Float; v.u_.f = f; return v; }
  static Value vector(float x, float y, float z) { Value v; v.t_ = VT::Vector; v.u_.v[0] = x; v.u_.v[1] = y; v.u_.v[2] = z; v.u_.v[3] = 0; return v; }
  static Value quat(float x, float y, float z, float w) { Value v; v.t_ = VT::Quat; v.u_.v[0] = x; v.u_.v[1] = y; v.u_.v[2] = z; v.u_.v[3] = w; return v; }
  static Value string(std::u16string s);
  static Value string(const char* ascii);
  static Value heap(VT t, HeapObj* p) { Value v; v.t_ = t; v.u_.p = p; if (p) p->addRef(); return v; }

  Value(const Value& o) : t_(o.t_), u_(o.u_) { if (isHeap()) u_.p->addRef(); }
  Value(Value&& o) noexcept : t_(o.t_), u_(o.u_) { o.t_ = VT::None; o.u_.p = nullptr; }
  Value& operator=(Value o) noexcept { std::swap(t_, o.t_); std::swap(u_, o.u_); return *this; }
  ~Value() { if (isHeap()) u_.p->release(); }

  VT type() const { return t_; }
  Cat category() const;
  bool isHeap() const { return t_ >= VT::String && t_ <= VT::Thread || t_ == VT::Ref; }
  bool isNone() const { return t_ == VT::None; }
  bool isNull() const { return t_ == VT::Null; }
  bool isNumber() const { return t_ == VT::Int || t_ == VT::Float; }

  bool b() const { return u_.b; }
  int32_t i() const { return u_.i; }
  float f() const { return u_.f; }
  const float* vec() const { return u_.v; }
  HeapObj* heapPtr() const { return isHeap() ? u_.p : nullptr; }

  const std::u16string& str() const;  // requires String
  ArrayObj* array() const { return t_ == VT::Array ? reinterpret_cast<ArrayObj*>(u_.p) : nullptr; }
  MapObj* map() const { return t_ == VT::Map ? reinterpret_cast<MapObj*>(u_.p) : nullptr; }
  Object* object() const { return t_ == VT::Object ? reinterpret_cast<Object*>(u_.p) : nullptr; }
  FuncObj* func() const { return t_ == VT::Function ? reinterpret_cast<FuncObj*>(u_.p) : nullptr; }
  MacroObj* macro() const { return t_ == VT::Macro ? reinterpret_cast<MacroObj*>(u_.p) : nullptr; }
  ThreadObj* thread() const { return t_ == VT::Thread ? reinterpret_cast<ThreadObj*>(u_.p) : nullptr; }
  RefObj* ref() const { return t_ == VT::Ref ? reinterpret_cast<RefObj*>(u_.p) : nullptr; }

  // Conversions (variant slots +0x1C toBool, +0x20 toInt, +0x24 toFloat, +0x28 toString).
  bool toBool() const;
  int32_t toInt() const;
  float toFloat() const;
  std::u16string toString() const;
  std::string toUtf8() const;
  // Identity/equality used by maps and '=='.
  bool sameAs(const Value& o) const;
  // Serialize as script source (variant slot +0x34; used by exportVariables).
  std::u16string exportSource() const;

 private:
  VT t_;
  union U {
    bool b;
    int32_t i;
    float f;
    float v[4];
    HeapObj* p;
  } u_;
};

class StrObj : public HeapObj {
 public:
  explicit StrObj(std::u16string s) : s(std::move(s)) {}
  std::u16string s;
};

class ArrayObj : public HeapObj {
 public:
  std::vector<Value> items;
};

// The engine's std::map<gsVariant*, gsVariant*> ordered by the '<' operator (values.md §5.4): numbers compare
// numerically (1 and 1.0 are one key), strings by code unit, objects by identity; categories without a '<' handler
// order by category number and collapse within it (true and false are one key).
bool mapKeyLess(const Value& a, const Value& b);

class MapObj : public HeapObj {
 public:
  std::vector<std::pair<Value, Value>> entries;  // sorted by mapKeyLess
  Value* find(const Value& key);
  Value& at(const Value& key);  // inserts unset when missing (the first key object is kept)
  bool erase(const Value& key);
};

class RefObj : public HeapObj {
 public:
  std::function<Value()> get;
  std::function<void(const Value&)> set;
  std::u16string describe;
};

std::u16string u16(const char* ascii);
// The engine prints floats with "%#.3f" everywhere: text, concatenation and export (values.md §2.3).
std::u16string formatFloat(float f);

}  // namespace regs::script
