#pragma once
// Minimal protocol buffers wire format (the Steam client's messages, core/steam_client.cpp): a writer that appends
// fields in order and a reader that walks the fields of one message. No schema: callers name the field numbers from
// the .proto definitions (SteamDatabase/Protobufs, docs/android.md).
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace regs::pb {

class Writer {
 public:
  std::vector<uint8_t> buf;

  void varint(uint64_t v) {
    while (v >= 0x80) {
      buf.push_back(uint8_t(v) | 0x80);
      v >>= 7;
    }
    buf.push_back(uint8_t(v));
  }
  void tag(int field, int wire) { varint((uint64_t(field) << 3) | uint64_t(wire)); }
  void u64(int field, uint64_t v) { tag(field, 0), varint(v); }
  void i32(int field, int32_t v) { u64(field, uint64_t(int64_t(v))); }  // negative ints take ten bytes, as protoc
  void boolean(int field, bool v) { u64(field, v ? 1 : 0); }
  void fixed64(int field, uint64_t v) {
    tag(field, 1);
    for (int i = 0; i < 8; ++i) buf.push_back(uint8_t(v >> (8 * i)));
  }
  void fixed32(int field, uint32_t v) {
    tag(field, 5);
    for (int i = 0; i < 4; ++i) buf.push_back(uint8_t(v >> (8 * i)));
  }
  void bytes(int field, std::span<const uint8_t> b) {
    tag(field, 2);
    varint(b.size());
    buf.insert(buf.end(), b.begin(), b.end());
  }
  void str(int field, std::string_view s) { bytes(field, {reinterpret_cast<const uint8_t*>(s.data()), s.size()}); }
  void msg(int field, const Writer& w) { bytes(field, w.buf); }
};

struct Field {
  int number = 0;
  int wire = 0;
  uint64_t value = 0;              // varint, fixed32 and fixed64 fields
  std::span<const uint8_t> data;   // length-delimited fields

  std::string str() const { return std::string(reinterpret_cast<const char*>(data.data()), data.size()); }
  std::vector<uint8_t> blob() const { return {data.begin(), data.end()}; }
  float f32() const {
    float f;
    uint32_t u = uint32_t(value);
    std::memcpy(&f, &u, 4);
    return f;
  }
};

// for (pb::Reader r(data); r.next();) switch (r.field.number) { ... }; ok() is false after malformed input
class Reader {
 public:
  explicit Reader(std::span<const uint8_t> d) : d_(d) {}
  Field field;

  bool next() {
    if (pos_ >= d_.size() || bad_) return false;
    uint64_t key;
    if (!varint(key)) return fail();
    field = Field{};
    field.number = int(key >> 3);
    field.wire = int(key & 7);
    switch (field.wire) {
      case 0:
        if (!varint(field.value)) return fail();
        return true;
      case 1:
        if (pos_ + 8 > d_.size()) return fail();
        for (int i = 0; i < 8; ++i) field.value |= uint64_t(d_[pos_ + i]) << (8 * i);
        pos_ += 8;
        return true;
      case 5:
        if (pos_ + 4 > d_.size()) return fail();
        for (int i = 0; i < 4; ++i) field.value |= uint64_t(d_[pos_ + i]) << (8 * i);
        pos_ += 4;
        return true;
      case 2: {
        uint64_t n;
        if (!varint(n) || n > d_.size() - pos_) return fail();
        field.data = d_.subspan(pos_, size_t(n));
        pos_ += size_t(n);
        return true;
      }
      default:
        return fail();
    }
  }
  bool ok() const { return !bad_; }

 private:
  bool varint(uint64_t& v) {
    v = 0;
    for (int shift = 0; shift < 64; shift += 7) {
      if (pos_ >= d_.size()) return false;
      uint8_t b = d_[pos_++];
      v |= uint64_t(b & 0x7f) << shift;
      if (!(b & 0x80)) return true;
    }
    return false;
  }
  bool fail() {
    bad_ = true;
    return false;
  }
  std::span<const uint8_t> d_;
  size_t pos_ = 0;
  bool bad_ = false;
};

}  // namespace regs::pb
