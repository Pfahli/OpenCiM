// Value conversions and export follow docs/script-runtime/values.md (§2.2 conversion matrix, §7 export format).
// These are the lenient versions for native code: unset converts to the type's zero and macros are not evaluated.
// The interpreter uses the strict versions in VM (unset raises "Cannot read valueless variant!", macros evaluate).
#include "script/value.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwctype>

#include "core/text.h"
#include "script/object.h"

namespace regs::script {

std::u16string u16(const char* ascii) {
  std::u16string s;
  while (*ascii) s += char16_t(uint8_t(*ascii++));
  return s;
}

Value Value::string(std::u16string s) { return heap(VT::String, new StrObj(std::move(s))); }
Value Value::string(const char* ascii) { return string(u16(ascii)); }

const std::u16string& Value::str() const { return reinterpret_cast<StrObj*>(u_.p)->s; }

Cat Value::category() const {
  switch (t_) {
    case VT::None: case VT::Macro: return Cat::None;
    case VT::Bool: return Cat::Bool;
    case VT::Int: return Cat::Int;
    case VT::Float: return Cat::Float;
    case VT::String: return Cat::String;
    case VT::Array: return Cat::Array;
    case VT::Map: return Cat::Map;
    case VT::Function: return Cat::Function;
    case VT::Vector: return Cat::Vector;
    case VT::Quat: return Cat::Quat;
    case VT::Ref: return ref()->get().category();
    default: return Cat::Object;  // null, objects, threads
  }
}

namespace {

float vecLength(const float* v) { return float(std::sqrt(double(v[2] * v[2] + v[1] * v[1] + v[0] * v[0]))); }

int32_t objectId(const Object* o) { return o ? int32_t(o->serial) : 0; }

// wistringstream >> int: skip white space, optional sign, decimal digits; stops at the first bad character.
bool parseInt(const std::u16string& s, int32_t& out) {
  size_t i = 0;
  while (i < s.size() && std::iswspace(s[i])) ++i;
  bool neg = false;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
  if (i >= s.size() || s[i] < '0' || s[i] > '9') return false;
  int64_t v = 0;
  while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
    v = v * 10 + (s[i++] - '0');
    if (v > 0x80000000LL) return false;  // overflow leaves the target unset in the engine
  }
  if (neg) v = -v;
  if (v > 0x7FFFFFFF || v < -0x80000000LL) return false;
  out = int32_t(v);
  return true;
}

bool parseFloat(const std::u16string& s, float& out) {
  std::string a;
  size_t i = 0;
  while (i < s.size() && std::iswspace(s[i])) ++i;
  for (; i < s.size() && s[i] < 0x80; ++i) a += char(s[i]);
  char* end = nullptr;
  double d = std::strtod(a.c_str(), &end);
  if (end == a.c_str()) return false;
  out = float(d);
  return true;
}

}  // namespace

std::u16string formatFloat(float f) {
  char buf[64];
  std::snprintf(buf, sizeof buf, "%#.3f", double(f));
  return u16(buf);
}

bool Value::toBool() const {
  switch (t_) {
    case VT::None: case VT::Null: case VT::Macro: case VT::Function: return false;
    case VT::Bool: return u_.b;
    case VT::Int: return u_.i != 0;
    case VT::Float: return u_.f != 0.0f || std::isnan(u_.f);
    case VT::String: {
      const auto& s = str();
      if (s == u"false" || s == u"0" || s.empty()) return false;
      return true;
    }
    case VT::Array: return !array()->items.empty();
    case VT::Map: return !map()->entries.empty();
    case VT::Vector: return u_.v[0] != 0 || u_.v[1] != 0 || u_.v[2] != 0;
    case VT::Quat: return !(u_.v[0] == 0 && u_.v[1] == 0 && u_.v[2] == 0 && u_.v[3] == 1);
    case VT::Ref: return ref()->get().toBool();
    default: return true;  // objects, threads
  }
}

int32_t Value::toInt() const {
  switch (t_) {
    case VT::Bool: return u_.b ? 1 : 0;
    case VT::Int: return u_.i;
    case VT::Float: {
      if (std::isnan(u_.f) || std::fabs(u_.f) >= 9.2233720368547758e18f) return 0;
      return int32_t(uint32_t(uint64_t(int64_t(u_.f))));  // _ftol2: truncate to 64 bits, keep the low 32
    }
    case VT::String: {
      if (str() == u"true") return 1;
      if (str() == u"false") return 0;
      int32_t v = 0;
      parseInt(str(), v);
      return v;
    }
    case VT::Array: return int32_t(array()->items.size());
    case VT::Map: return int32_t(map()->entries.size());
    case VT::Vector: return int32_t(vecLength(u_.v));
    case VT::Quat: return int32_t(u_.v[3]);
    case VT::Object: return objectId(object());
    case VT::Ref: return ref()->get().toInt();
    default: return 0;
  }
}

float Value::toFloat() const {
  switch (t_) {
    case VT::Bool: return u_.b ? 1.0f : 0.0f;
    case VT::Int: return float(u_.i);
    case VT::Float: return u_.f;
    case VT::String: {
      if (str() == u"true") return 1.0f;
      if (str() == u"false") return 0.0f;
      float v = 0;
      parseFloat(str(), v);
      return v;
    }
    case VT::Array: return float(array()->items.size());
    case VT::Map: return float(map()->entries.size());
    case VT::Vector: return vecLength(u_.v);
    case VT::Quat: return u_.v[3];
    case VT::Object: return float(objectId(object()));
    case VT::Ref: return ref()->get().toFloat();
    default: return 0.0f;
  }
}

std::u16string Value::toString() const {
  switch (t_) {
    case VT::None: case VT::Macro: case VT::Function: return u"";
    case VT::Null: return u"null";
    case VT::Bool: return u_.b ? u"true" : u"false";
    case VT::Int: return u16(std::to_string(u_.i).c_str());
    case VT::Float: return formatFloat(u_.f);
    case VT::String: return str();
    case VT::Vector: return u"(" + formatFloat(u_.v[0]) + u", " + formatFloat(u_.v[1]) + u", " + formatFloat(u_.v[2]) + u")";
    case VT::Quat:
      return u"(" + formatFloat(u_.v[0]) + u", " + formatFloat(u_.v[1]) + u", " + formatFloat(u_.v[2]) + u", " + formatFloat(u_.v[3]) + u")";
    case VT::Array: {  // concatenation of the elements, no separator
      std::u16string s;
      for (auto& v : array()->items) s += v.toString();
      return s;
    }
    case VT::Map: {  // key export + value text per entry, no separator
      std::u16string s;
      for (auto& [k, v] : map()->entries) s += k.exportSource() + v.toString();
      return s;
    }
    case VT::Object: return u16(std::to_string(objectId(object())).c_str());
    case VT::Ref: return ref()->get().toString();
    default: return u"";
  }
}

std::string Value::toUtf8() const { return utf16ToUtf8(toString()); }

bool Value::sameAs(const Value& o) const {
  if (t_ == VT::Ref) return ref()->get().sameAs(o);
  if (o.t_ == VT::Ref) return sameAs(o.ref()->get());
  if (t_ != o.t_) {
    if (isNumber() && o.isNumber()) return toFloat() == o.toFloat();
    return false;
  }
  switch (t_) {
    case VT::None: case VT::Null: return true;
    case VT::Bool: return u_.b == o.u_.b;
    case VT::Int: return u_.i == o.u_.i;
    case VT::Float: return u_.f == o.u_.f;
    case VT::String: return str() == o.str();
    case VT::Vector: return u_.v[0] == o.u_.v[0] && u_.v[1] == o.u_.v[1] && u_.v[2] == o.u_.v[2];
    case VT::Quat: return u_.v[0] == o.u_.v[0] && u_.v[1] == o.u_.v[1] && u_.v[2] == o.u_.v[2] && u_.v[3] == o.u_.v[3];
    default: return u_.p == o.u_.p;
  }
}

std::u16string Value::exportSource() const {
  switch (t_) {
    case VT::None: case VT::Macro: return u"0 /* cannot export variant */";
    case VT::Null: return u"null";
    case VT::Bool: return u_.b ? u"true" : u"false";
    case VT::String: {
      std::u16string s = u"\"";
      for (char16_t c : str()) {
        if (c == 0) break;
        switch (c) {
          case '\t': s += u"\\t"; break;
          case '\n': s += u"\\n"; break;
          case '\r': s += u"\\r"; break;
          case '"': s += u"\\\""; break;
          case '\\': s += u"\\\\"; break;
          default:
            if (c >= 0x80) {
              char buf[8];
              std::snprintf(buf, sizeof buf, "\\u%04X", unsigned(c));
              s += u16(buf);
            } else {
              s += c;
            }
        }
      }
      return s + u"\"";
    }
    case VT::Array: {
      std::u16string s = u"[";
      auto& items = array()->items;
      for (size_t i = 0; i < items.size(); ++i) s += (i ? u", " : u"") + items[i].exportSource();
      return s + u"]";
    }
    case VT::Map: {
      std::u16string s = u"map [";
      bool first = true;
      for (auto& [k, v] : map()->entries) {
        s += (first ? u"" : u", ") + k.exportSource() + u", " + v.exportSource();
        first = false;
      }
      return s + u"]";
    }
    case VT::Vector: return u"vector(" + formatFloat(u_.v[0]) + u", " + formatFloat(u_.v[1]) + u", " + formatFloat(u_.v[2]) + u")";
    case VT::Quat:
      return u"quat(" + formatFloat(u_.v[0]) + u", " + formatFloat(u_.v[1]) + u", " + formatFloat(u_.v[2]) + u", " + formatFloat(u_.v[3]) + u")";
    case VT::Function: return u"null /* cannot export function */";
    case VT::Object: case VT::Thread: return u"null /* cannot export object */";
    case VT::Ref: return ref()->get().exportSource();
    default: return toString();
  }
}

bool mapKeyLess(const Value& ra, const Value& rb) {
  const Value& a = ra.type() == VT::Ref ? ra : ra;
  Cat ca = a.category(), cb = rb.category();
  bool na = ca == Cat::Int || ca == Cat::Float, nb = cb == Cat::Int || cb == Cat::Float;
  if (na && nb) {
    if (ca == Cat::Int && cb == Cat::Int) return a.toInt() < rb.toInt();
    return a.toFloat() < rb.toFloat();
  }
  if (ca == Cat::String && cb == Cat::String) return a.toString() < rb.toString();
  if (ca == Cat::Object && cb == Cat::Object) {
    // identity order; the engine uses addresses, the port uses creation order for determinism
    auto id = [](const Value& v) -> uint64_t {
      if (Object* o = v.object()) return o->serial;
      if (v.isNull()) return 0;
      return uint64_t(reinterpret_cast<uintptr_t>(v.heapPtr()));
    };
    return id(a) < id(rb);
  }
  return uint8_t(ca) < uint8_t(cb);
}

Value* MapObj::find(const Value& key) {
  auto it = std::lower_bound(entries.begin(), entries.end(), key,
                             [](const std::pair<Value, Value>& e, const Value& k) { return mapKeyLess(e.first, k); });
  if (it != entries.end() && !mapKeyLess(key, it->first)) return &it->second;
  return nullptr;
}

Value& MapObj::at(const Value& key) {
  auto it = std::lower_bound(entries.begin(), entries.end(), key,
                             [](const std::pair<Value, Value>& e, const Value& k) { return mapKeyLess(e.first, k); });
  if (it != entries.end() && !mapKeyLess(key, it->first)) return it->second;
  it = entries.emplace(it, key, Value());
  return it->second;
}

bool MapObj::erase(const Value& key) {
  auto it = std::lower_bound(entries.begin(), entries.end(), key,
                             [](const std::pair<Value, Value>& e, const Value& k) { return mapKeyLess(e.first, k); });
  if (it != entries.end() && !mapKeyLess(key, it->first)) {
    entries.erase(it);
    return true;
  }
  return false;
}

}  // namespace regs::script
