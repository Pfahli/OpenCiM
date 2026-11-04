// Operators, strict conversions and container access, following docs/script-runtime/values.md and the dispatch
// table extracted from GSystem.dll (operators_table.inc, generated from docs/script-runtime/operators.json).
#include <cmath>
#include <cstring>
#include <limits>

#include "core/text.h"
#include "script/vm.h"

namespace regs::script {

namespace {
#include "script/operators_table.inc"

Value plain(const Value& v) { return v.ref() ? v.ref()->get() : v; }

int32_t wrapAdd(int32_t a, int32_t b) { return int32_t(uint32_t(a) + uint32_t(b)); }
int32_t wrapSub(int32_t a, int32_t b) { return int32_t(uint32_t(a) - uint32_t(b)); }
int32_t wrapMul(int32_t a, int32_t b) { return int32_t(uint32_t(a) * uint32_t(b)); }

// Hamilton product on (x, y, z, w)
void qmul(const float* a, const float* b, float* o) {
  float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
  float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
  float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
  float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
  o[0] = x; o[1] = y; o[2] = z; o[3] = w;
}

bool identical(const Value& a, const Value& b) {
  if (a.isNull() && b.isNull()) return true;
  if (a.type() != b.type()) return false;
  if (a.isHeap()) return a.heapPtr() == b.heapPtr();
  return false;  // distinct value objects (ints, floats ...) are never the same object
}

uint64_t objectOrder(const Value& v) {
  if (Object* o = v.object()) return o->serial;
  if (v.isNull()) return 0;
  return uint64_t(reinterpret_cast<uintptr_t>(v.heapPtr()));
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------
// strict conversions (values.md §2.2): unset raises error 5, macros evaluate first

Value VM::resolve(const Value& v) {
  Value x = plain(v);
  if (MacroObj* m = x.macro()) return evalMacro(m);
  return x;
}

bool VM::truthy(const Value& v) {
  Value x = resolve(v);
  if (x.isNone()) error(kErrValueless, "Cannot read valueless variant!");
  return x.toBool();
}

int32_t VM::toInt(const Value& v) {
  Value x = resolve(v);
  if (x.isNone()) error(kErrValueless, "Cannot read valueless variant!");
  return x.toInt();
}

float VM::toFloat(const Value& v) {
  Value x = resolve(v);
  if (x.isNone()) error(kErrValueless, "Cannot read valueless variant!");
  return x.toFloat();
}

std::u16string VM::toStr(const Value& v) {
  Value x = resolve(v);
  if (x.isNone()) error(kErrValueless, "Cannot read valueless variant!");
  return x.toString();
}

// ---------------------------------------------------------------------------------------------------------------
// operators

Value VM::binop(Op op, const Value& a, const Value& b) {
  Value l = plain(a), r = plain(b);
  uint8_t h = kOpTable[uint8_t(op)][uint8_t(l.category())][uint8_t(r.category())];
  if (!h) error(kErrOperator, "Operator " + std::to_string(int(op)) + " not defined for operands!");
  return applyHandler(h, op, l, r);
}

Value VM::unop(Op op, const Value& a) {
  Value r = plain(a);
  if (op == Op::Ref) {  // '@' of a non-reference (lvalues compile to Ref* opcodes)
    if (r.ref()) return a;
    error(kErrNoReference, "Variant cannot be referenced!");
  }
  uint8_t h = kOpTable[uint8_t(op)][0][uint8_t(r.category())];
  if (!h) error(kErrOperator, "Operator " + std::to_string(int(op)) + " not defined for operands!");
  return applyHandler(h, op, Value(), r);
}

// Compound assignment / ++ / --: the cell of the compound (or increment) operator decides whether it is defined,
// keyed on the current value of the target (values.md §4.9-4.10).
Value VM::update(Op baseOp, uint8_t upd, const Value& rawCur, const Value& rhs) {
  Value cur = plain(rawCur);
  if (upd >= 0xF0) {  // ++ / -- : int only
    bool pre = upd == 0xF0 || upd == 0xF1;
    bool inc = upd == 0xF0 || upd == 0xF2;
    Op o = pre ? (inc ? Op::PreInc : Op::PreDec) : (inc ? Op::PostInc : Op::PostDec);
    uint8_t h = pre ? kOpTable[uint8_t(o)][0][uint8_t(cur.category())] : kOpTable[uint8_t(o)][uint8_t(cur.category())][0];
    if (!h) error(kErrOperator, "Operator " + std::to_string(int(o)) + " not defined for operands!");
    return Value::integer(inc ? wrapAdd(cur.toInt(), 1) : wrapSub(cur.toInt(), 1));
  }
  // compound: upd is the base binary operator; the compound operator index is base + 0x1C (0x1D for +=, ...)
  static const uint8_t compound[0x15] = {0, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0, 0, 0, 0, 0, 0, 0x25, 0x26, 0x27, 0, 0, 0, 0x22, 0x23, 0x24};
  uint8_t cop = upd < sizeof compound ? compound[upd] : 0;
  Value r = plain(rhs);
  uint8_t h = cop ? kOpTable[cop][uint8_t(cur.category())][uint8_t(r.category())] : 0;
  if (!h) error(kErrOperator, "Operator " + std::to_string(int(cop)) + " not defined for operands!");
  return applyHandler(h, baseOp, cur, r);
}

Value VM::applyHandler(uint8_t h, Op op, const Value& l, const Value& r) {
  switch (h) {
    case H_10011470: return r;                                               // unary +
    case H_10011480: { bool a = truthy(l); return Value::boolean(a && truthy(r)); }
    case H_100114b0: { bool a = truthy(l); return Value::boolean(a || truthy(r)); }
    case H_100114e0: return Value::boolean(!truthy(r));
    case H_10011500: return truthy(l) ? r : Value();                          // ?
    case H_10011520: return l.isNone() ? r : l;                               // :
    case H_10011540:                                                          // @
      if (r.ref()) return r;
      error(kErrNoReference, "Variant cannot be referenced!");
    // int arithmetic (plain and compound share the computation)
    case H_10011970: case H_100119d0: return Value::integer(wrapAdd(toInt(l), toInt(r)));
    case H_10011a50: case H_10011ab0: return Value::integer(wrapSub(toInt(l), toInt(r)));
    case H_10011b30: case H_10011b90: return Value::integer(wrapMul(toInt(l), toInt(r)));
    case H_10011c10: case H_10011cc0: {
      int32_t d = toInt(r);
      if (d == 0) error(kErrDivZero, "Division by zero!");
      int32_t n = toInt(l);
      if (n == std::numeric_limits<int32_t>::min() && d == -1) return Value::integer(n);
      return Value::integer(n / d);
    }
    case H_10011da0: case H_10011e80: {
      int32_t d = toInt(r);
      if (d == 0) error(kErrDivZero, "Division by zero!");
      int32_t n = toInt(l);
      if (d == -1) return Value::integer(0);
      return Value::integer(n % d);
    }
    // float arithmetic
    case H_100119a0: case H_10011a10: return Value::real(toFloat(l) + toFloat(r));
    case H_10011a80: case H_10011af0: return Value::real(toFloat(l) - toFloat(r));
    case H_10011b60: case H_10011bd0: return Value::real(toFloat(l) * toFloat(r));
    case H_10011c60: case H_10011d20: {
      float d = toFloat(r);
      if (d == 0.0f) error(kErrDivZero, "Division by zero!");
      return Value::real(toFloat(l) / d);
    }
    case H_10011df0: case H_10011ee0: {
      float d = toFloat(r);
      if (d == 0.0f) error(kErrDivZero, "Division by zero!");
      return Value::real(float(std::fmod(double(toFloat(l)), double(d))));
    }
    case H_10011f80: return Value::integer(wrapSub(0, toInt(r)));
    case H_10011fa0: return Value::real(-toFloat(r));
    case H_10011fc0: case H_10012020: return Value::integer(wrapAdd(toInt(op == Op::PostInc ? l : r), 1));
    case H_10011ff0: case H_10012050: return Value::integer(wrapSub(toInt(op == Op::PostDec ? l : r), 1));
    // equality
    case H_10012080: return Value::boolean(truthy(l) == truthy(r));
    case H_10012180: return Value::boolean(truthy(l) != truthy(r));
    case H_100120b0: return Value::boolean(toInt(l) == toInt(r));
    case H_100121b0: return Value::boolean(toInt(l) != toInt(r));
    case H_100120e0: return Value::boolean(toFloat(l) == toFloat(r));
    case H_100121e0: return Value::boolean(!(toFloat(l) == toFloat(r)));
    case H_10012140: return Value::boolean(identical(l, r));
    case H_10012240: return Value::boolean(!identical(l, r));
    case H_100129f0: { auto b = toStr(r); return Value::boolean(toStr(l) == b); }
    case H_10012ae0: { auto b = toStr(r); return Value::boolean(toStr(l) != b); }
    // ordering
    case H_10012280: return Value::boolean(toInt(l) < toInt(r));
    case H_10012310: return Value::boolean(toInt(l) <= toInt(r));
    case H_100123a0: return Value::boolean(toInt(l) > toInt(r));
    case H_10012430: return Value::boolean(toInt(l) >= toInt(r));
    case H_100122b0: return Value::boolean(toFloat(l) < toFloat(r));
    case H_10012340: return Value::boolean(toFloat(l) <= toFloat(r));
    case H_100123d0: return Value::boolean(toFloat(l) > toFloat(r));
    case H_10012460: return Value::boolean(toFloat(l) >= toFloat(r));
    case H_10012bc0: return Value::boolean(toStr(l) < toStr(r));
    case H_10012ca0: return Value::boolean(toStr(l) <= toStr(r));
    case H_10012d80: return Value::boolean(toStr(l) > toStr(r));
    case H_10012e60: return Value::boolean(toStr(l) >= toStr(r));
    case H_100122e0: return Value::boolean(objectOrder(l) < objectOrder(r));
    case H_10012370: return Value::boolean(objectOrder(l) <= objectOrder(r));
    case H_10012400: return Value::boolean(objectOrder(l) > objectOrder(r));
    case H_10012490: return Value::boolean(objectOrder(l) >= objectOrder(r));
    // bits and shifts (int only)
    case H_100124c0: case H_100124f0: return Value::integer(toInt(l) & toInt(r));
    case H_10012530: case H_10012560: return Value::integer(toInt(l) | toInt(r));
    case H_100125a0: case H_100125d0: return Value::integer(toInt(l) ^ toInt(r));
    case H_10012610: return Value::integer(~toInt(r));
    case H_10012630: case H_10012660: { int32_t a = toInt(l); return Value::integer(int32_t(uint32_t(a) << (toInt(r) & 31))); }
    case H_100126a0: case H_100126d0: { int32_t a = toInt(l); return Value::integer(a >> (toInt(r) & 31)); }
    case H_10012710: case H_10012740: { int32_t a = toInt(l); return Value::integer(int32_t(uint32_t(a) >> (toInt(r) & 31))); }
    // '#'
    case H_10012780: {
      Value x = resolve(r);
      if (x.type() == VT::String) return Value::integer(int32_t(x.str().size()));
      if (ArrayObj* arr = x.array()) return Value::integer(int32_t(arr->items.size()));
      if (MapObj* m = x.map()) return Value::integer(int32_t(m->entries.size()));
      if (Object* o = x.object(); o && isModule(o)) return Value::integer(int32_t(o->children.size()));
      error(kErrNotIndexable, "Variant not indexable!");
    }
    // strings
    case H_100127a0: case H_100128c0: { auto b = toStr(r); return Value::string(toStr(l) + b); }
    // vectors and quaternions
    case H_10092b40: case H_10092be0: { auto a = l.vec(), b = r.vec(); return Value::vector(a[0] + b[0], a[1] + b[1], a[2] + b[2]); }
    case H_10092c90: case H_10092d30: { auto a = l.vec(), b = r.vec(); return Value::vector(a[0] - b[0], a[1] - b[1], a[2] - b[2]); }
    case H_10092de0: case H_10092e80: { auto a = l.vec(), b = r.vec(); return Value::vector(a[0] * b[0], a[1] * b[1], a[2] * b[2]); }
    case H_10092f30: case H_10092fd0: {
      auto a = l.vec(), b = r.vec();
      auto dv = [](float x, float y) { return y == 0.0f ? 0.0f : x / y; };
      return Value::vector(dv(a[0], b[0]), dv(a[1], b[1]), dv(a[2], b[2]));
    }
    case H_10093080: { auto a = r.vec(); return Value::vector(-a[0], -a[1], -a[2]); }
    case H_10093890: case H_100938f0: {
      auto a = l.vec(), b = r.vec();
      bool eq = a[0] == b[0] && a[1] == b[1] && a[2] == b[2];
      return Value::boolean(h == H_10093890 ? eq : !eq);
    }
    case H_10093950: case H_100939b0: {
      auto a = l.vec(), b = r.vec();
      bool eq = a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
      return Value::boolean(h == H_10093950 ? eq : !eq);
    }
    case H_10093100: { float o[4]; qmul(l.vec(), r.vec(), o); return Value::quat(o[0], o[1], o[2], o[3]); }
    case H_100931a0: {  // conj(R) * L
      auto q = r.vec();
      float c[4] = {-q[0], -q[1], -q[2], q[3]}, o[4];
      qmul(c, l.vec(), o);
      return Value::quat(o[0], o[1], o[2], o[3]);
    }
    case H_10093240: {  // (conj(L) * (R, 0) * L).xyz
      auto q = l.vec(), v = r.vec();
      float c[4] = {-q[0], -q[1], -q[2], q[3]}, p[4] = {v[0], v[1], v[2], 0}, t[4], o[4];
      qmul(c, p, t);
      qmul(t, q, o);
      return Value::vector(o[0], o[1], o[2]);
    }
    default: break;
  }
  error(kErrOperator, "Operator " + std::to_string(int(op)) + " not defined for operands!");
}

// ---------------------------------------------------------------------------------------------------------------
// containers (values.md §5.3-5.4)

void VM::storeSlot(Value& slot, const Value& v) {
  Value x = plain(v);
  if (RefObj* r = slot.ref()) {  // an '@' alias writes through to its target
    r->set(x);
    return;
  }
  slot = x;
}

Value VM::indexGet(const Value& ra, const Value& ri) {
  Value a = plain(ra);
  if (ArrayObj* arr = a.array()) {
    int32_t i = toInt(ri);
    int64_t n = int64_t(arr->items.size());
    int64_t k = i < 0 ? n + i : i;
    if (k < 0 || k >= n) error(kErrArrayIndex, "Array index out of bounds (" + std::to_string(i) + ")!");
    return plain(arr->items[size_t(k)]);
  }
  if (MapObj* m = a.map()) {
    Value key = resolve(ri);
    if (Value* x = m->find(key)) return plain(*x);
    error(kErrMapIndex, "Map index out of bounds (" + utf16ToUtf8(key.exportSource()) + ")!");
  }
  error(kErrNotIndexable, "Variant not indexable!");
}

void VM::indexSet(const Value& ra, const Value& ri, const Value& v) {
  Value a = plain(ra);
  if (ArrayObj* arr = a.array()) {
    int32_t i = toInt(ri);
    int64_t n = int64_t(arr->items.size());
    int64_t k = i < 0 ? n + i : i;
    if (k < 0) error(kErrArrayIndex, "Array index out of bounds (" + std::to_string(i) + ")!");
    if (k >= n) arr->items.resize(size_t(k) + 1);  // grows, filling with unset
    storeSlot(arr->items[size_t(k)], v);
    return;
  }
  if (MapObj* m = a.map()) {
    storeSlot(m->at(resolve(ri)), v);
    return;
  }
  error(kErrNotIndexable, "Variant not indexable!");
}

bool VM::indexDefined(const Value& ra, const Value& ri) {
  Value a = plain(ra);
  if (ArrayObj* arr = a.array()) {
    int32_t i = toInt(ri);
    int64_t n = int64_t(arr->items.size());
    int64_t k = i < 0 ? n + i : i;
    return k >= 0 && k < n && !plain(arr->items[size_t(k)]).isNone();
  }
  if (MapObj* m = a.map()) return m->find(resolve(ri)) != nullptr;
  error(kErrNotIndexable, "Variant not indexable!");
}

}  // namespace regs::script
