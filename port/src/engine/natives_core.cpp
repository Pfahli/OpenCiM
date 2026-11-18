// Core global functions and gsScript/gsObjectVariant methods. Semantics follow docs/handlers/batch-C.md.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <filesystem>

#include "build_info.h"
#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/gui.h"
#include "engine/host.h"
#include "engine/platform.h"
#include "engine/user_folders.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::script;
namespace fs = std::filesystem;

namespace {

std::string u8(const std::u16string& s) { return utf16ToUtf8(s); }
std::u16string w(const std::string& s) { return utf8ToUtf16(s); }
Value str(const std::u16string& s) { return Value::string(s); }
Value deref(const Value& v) { return v.ref() ? v.ref()->get() : v; }
bool isFloat(const Value& v) { return deref(v).type() == VT::Float; }

int32_t rand31() { return int32_t((uint32_t(std::rand()) << 16) | (uint32_t(std::rand()) & 0xFFFF)); }

// mkdate including the engine's year-offset bug for years 101-103 mod 100-400 (docs/handlers/batch-C.md)
int32_t mkdate(int day, int month, int year) {
  static const int cum[13] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365};
  int q = year / 400;
  if (year % 400 < 0) --q;
  int r = year - 400 * q;
  int base = q * 146097;
  int rem;
  if (r < 100) {
    base += (r / 4) * 1461;
    rem = r % 4;
  } else {
    base += 36525 + ((r - 100) / 100) * 36524;
    int yc = (r - 100) % 100;
    if (yc >= 4) {
      base += 1460 + ((yc - 4) / 4) * 1461;
      rem = (yc - 4) % 4;
    } else {
      rem = yc == 0 ? 0 : -1;  // BUG kept: years 1..3 into the century skip their offset
    }
  }
  bool leap = rem == 0;
  if (rem > 0) base += 1 + rem * 365;
  if (rem < 0) leap = false;
  if (month < 1 || month > 12) month = std::clamp(month, 1, 12);
  int mdays = cum[month] - cum[month - 1] + ((leap && month == 2) ? 1 : 0);
  if (day > mdays) day = mdays;
  int doy = cum[month - 1] + day - 1 + ((leap && month > 2) ? 1 : 0);
  return base + doy;
}


std::u16string extractName(const std::u16string& p) {
  size_t k = p.find_last_of(u"/\\");
  return k == std::u16string::npos ? p : p.substr(k + 1);
}

Value array(std::vector<Value> items) {
  auto a = make<ArrayObj>();
  a->items = std::move(items);
  return Value::heap(VT::Array, a.get());
}

std::vector<std::u16string> namesFrom(int argc, const Value* args) {
  std::vector<std::u16string> names;
  for (int i = 0; i < argc; ++i) {
    Value a = deref(args[i]);
    if (ArrayObj* arr = a.array()) for (auto& x : arr->items) names.push_back(deref(x).toString());
    else names.push_back(a.toString());
  }
  return names;
}

// exportVariablesTo core (0x10018340): template = the script's own source, '$name ' lines replaced or appended
void exportVariables(VM& vm, Object* self, const std::u16string& dest, int argc, const Value* args) {
  std::vector<uint8_t> data;
  std::vector<std::u16string> lines;
  if (vm.vfs().read(u8(self->scriptPath), data) && !data.empty()) {
    std::u16string text = decodeTextFile(data);
    std::u16string cur;
    for (char16_t c : text) {
      if (c == '\n' || c == 0) {
        if (!cur.empty() && cur.back() == '\r') cur.pop_back();
        lines.push_back(cur);
        cur.clear();
      } else {
        cur += c;
      }
    }
    lines.push_back(cur);
    if (!lines.empty() && lines.back().empty()) lines.pop_back();
  }
  size_t templateLines = lines.size();
  for (auto& name : namesFrom(argc, args)) {
    Value* v = self->findVar(name);
    if (!v) continue;
    std::u16string line = u"$" + name + u" = " + v->exportSource() + u";";
    std::u16string prefix = u"$" + name + u" ";
    bool replaced = false;
    for (size_t i = 0; i < templateLines; ++i)
      if (lines[i].rfind(prefix, 0) == 0) {
        lines[i] = line;
        replaced = true;
        break;
      }
    if (!replaced) lines.push_back(line);
  }
  std::string out;
  for (auto& l : lines) {
    for (char16_t c : l) out += c < 0x100 ? char(c) : '?';
    out += "\r\n";
  }
  if (!vm.vfs().write(u8(dest), std::vector<uint8_t>(out.begin(), out.end())))
    LOG_WARN("exportVariables: cannot write %s", u8(dest).c_str());
}

}  // namespace

void registerCoreNatives(VM& vm) {
  // ---- the port's version (build_info.h, cmake/build_info.cmake): shown in the main menu's corner ----
  vm.setGlobal(u"REGS_VERSION", Value::string(utf8ToUtf16(REGS_VERSION_STRING)));
  vm.setGlobal(u"REGS_BUILD", Value::integer(REGS_BUILD_NUMBER));
  vm.setGlobal(u"REGS_COMMIT", Value::string(utf8ToUtf16(REGS_BUILD_COMMIT)));
  vm.setGlobal(u"REGS_BUILD_DATE", Value::string(utf8ToUtf16(REGS_BUILD_DATE)));
  // ---- output ----
  implementGlobal(vm, u"print", [](VM& v, Object*, int argc, const Value* a) {
    std::string s;
    for (int i = 0; i < argc; ++i) s += deref(a[i]).toUtf8();
    if (v.output) v.output(s);
    return Value();
  });

  // ---- math ----
  implementGlobal(vm, u"random", [](VM& v, Object*, int argc, const Value* a) {
    if (argc == 0) return Value::real(float(std::rand()) / 32767.0f);
    if (argc == 1) {
      if (isFloat(a[0])) {
        float x = deref(a[0]).f();
        if (x < 0) v.error("Calling a function with invalid argument(s)!");
        return Value::real(float(std::rand()) * x / 32767.0f);
      }
      int32_t n = deref(a[0]).toInt();
      if (n < 1) v.error("Calling a function with invalid argument(s)!");
      return Value::integer(rand31() % n);
    }
    if (isFloat(a[0]) || isFloat(a[1])) {
      float lo = deref(a[0]).toFloat(), hi = deref(a[1]).toFloat();
      if (hi < lo) v.error("Calling a function with invalid argument(s)!");
      return Value::real(float(std::rand()) * (hi - lo) / 32767.0f + lo);
    }
    int32_t lo = deref(a[0]).toInt(), hi = deref(a[1]).toInt();
    if (hi < lo) v.error("Calling a function with invalid argument(s)!");
    return Value::integer(lo + rand31() % (hi - lo + 1));
  });
  auto cmpArgs = [](const Value& x, const Value& y) {  // <0, 0, >0 like the engine's min/max
    if (isFloat(x) || isFloat(y)) {
      float p = deref(x).toFloat(), q = deref(y).toFloat();
      return p < q ? -1 : p > q ? 1 : 0;
    }
    int32_t p = deref(x).toInt(), q = deref(y).toInt();
    return p < q ? -1 : p > q ? 1 : 0;
  };
  implementGlobal(vm, u"max", [cmpArgs](VM&, Object*, int, const Value* a) { return cmpArgs(a[0], a[1]) >= 0 ? deref(a[0]) : deref(a[1]); });
  implementGlobal(vm, u"min", [cmpArgs](VM&, Object*, int, const Value* a) { return cmpArgs(a[0], a[1]) <= 0 ? deref(a[0]) : deref(a[1]); });
  implementGlobal(vm, u"clamp", [cmpArgs](VM&, Object*, int, const Value* a) {
    if (cmpArgs(a[0], a[1]) < 0) return deref(a[1]);
    if (cmpArgs(a[2], a[0]) < 0) return deref(a[2]);
    return deref(a[0]);
  });
  implementGlobal(vm, u"int", [](VM&, Object*, int, const Value* a) {
    Value x = deref(a[0]);
    if (x.type() == VT::String) {
      if (x.str() == u"true") return Value::integer(1);
      if (x.str() == u"false") return Value::integer(0);
    }
    return Value::integer(x.toInt());
  });
  implementGlobal(vm, u"float", [](VM&, Object*, int, const Value* a) { return Value::real(deref(a[0]).toFloat()); });
  implementGlobal(vm, u"bool", [](VM&, Object*, int, const Value* a) { return Value::boolean(deref(a[0]).toBool()); });
  implementGlobal(vm, u"string", [](VM&, Object*, int, const Value* a) { return str(deref(a[0]).toString()); });
  implementGlobal(vm, u"abs", [](VM&, Object*, int, const Value* a) {
    Value x = deref(a[0]);
    return x.type() == VT::Float ? Value::real(std::fabs(x.f())) : Value::integer(std::abs(x.toInt()));
  });
  implementGlobal(vm, u"sign", [](VM&, Object*, int, const Value* a) {
    float x = deref(a[0]).toFloat();
    return Value::integer(x > 0 ? 1 : x < 0 ? -1 : 0);
  });
  implementGlobal(vm, u"sin", [](VM&, Object*, int, const Value* a) { return Value::real(std::sin(deref(a[0]).toFloat())); });
  implementGlobal(vm, u"cos", [](VM&, Object*, int, const Value* a) { return Value::real(std::cos(deref(a[0]).toFloat())); });
  implementGlobal(vm, u"tan", [](VM&, Object*, int, const Value* a) { return Value::real(std::tan(deref(a[0]).toFloat())); });
  // arg(x, y) (gsFunction_arg L:0x0824c9b0): the angle of (x, y) in [0, 2π), via acos of the normalised x
  implementGlobal(vm, u"arg", [](VM&, Object*, int, const Value* a) {
    float x = deref(a[0]).toFloat(), y = deref(a[1]).toFloat();
    float n = std::sqrt(x * x + y * y), inv = n > 0 ? 1.0f / n : n;
    float c = x * inv;
    if (c >= 1) return Value::real(0.0f);
    if (c <= -1) return Value::real(3.1415927f);
    return Value::real(y * inv >= 0 ? std::acos(c) : 6.2831855f - std::acos(c));
  });
  implementGlobal(vm, u"pow",[](VM&, Object*, int, const Value* a) { return Value::real(std::pow(deref(a[0]).toFloat(), deref(a[1]).toFloat())); });
  implementGlobal(vm, u"vector", [](VM& v, Object*, int argc, const Value* a) {
    if (argc == 3) return Value::vector(deref(a[0]).toFloat(), deref(a[1]).toFloat(), deref(a[2]).toFloat());
    if (argc == 1 && deref(a[0]).type() == VT::Vector) return deref(a[0]);
    if (argc == 2) v.error("Wrong number of arguments (2)!");
    return Value::vector(0, 0, 0);
  });
  implementGlobal(vm, u"vector_length", [](VM&, Object*, int, const Value* a) {
    Value x = deref(a[0]);
    if (x.type() != VT::Vector) return Value::real(0);
    const float* p = x.vec();
    return Value::real(std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]));
  });
  for (int k = 0; k < 3; ++k) {
    std::u16string n = std::u16string(u"vector_") + char16_t(u'x' + k);
    implementGlobal(vm, n, [k](VM&, Object*, int, const Value* a) {
      Value x = deref(a[0]);
      return x.type() == VT::Vector ? Value::real(x.vec()[k]) : Value::real(0);
    });
  }
  implementGlobal(vm, u"quat", [](VM& v, Object*, int argc, const Value* a) {
    if (argc == 4) return Value::quat(deref(a[0]).toFloat(), deref(a[1]).toFloat(), deref(a[2]).toFloat(), deref(a[3]).toFloat());
    if (argc == 2) {
      float ang = deref(a[0]).toFloat();
      Value ax = deref(a[1]);
      float x = 0, y = 0, z = 0;
      if (ax.type() == VT::Vector) { x = ax.vec()[0]; y = ax.vec()[1]; z = ax.vec()[2]; }
      float len = std::sqrt(x * x + y * y + z * z);
      if (len > 0) { x /= len; y /= len; z /= len; }
      float s = std::sin(ang * 0.5f);
      return Value::quat(x * s, y * s, z * s, std::cos(ang * 0.5f));
    }
    if (argc == 1 && deref(a[0]).type() == VT::Quat) return deref(a[0]);
    if (argc == 3) v.error("Wrong number of arguments (3)!");
    return Value::quat(0, 0, 0, 1);
  });
  // setRunSpeed(group, speed) (0x1000c320 → 0x1002c5a0, semantics.md §5.2): the thread group's time runs at speed
  // (the game's speed buttons set "game" to 0, 1, 4 or 10); a group named here for the first time is created paused
  implementGlobal(vm, u"setRunSpeed", [](VM& v, Object*, int, const Value* a) {
    v.group(deref(a[0]).toString()).speed = deref(a[1]).toFloat();
    return Value();
  });
  implementGlobal(vm, u"mkdate", [](VM&, Object*, int, const Value* a) {
    return Value::integer(mkdate(deref(a[0]).toInt(), deref(a[1]).toInt(), deref(a[2]).toInt()));
  });
  implementGlobal(vm, u"mktime", [](VM&, Object*, int, const Value* a) {
    return Value::integer(((deref(a[0]).toInt() * 60 + deref(a[1]).toInt()) * 60 + deref(a[2]).toInt()) * 1000);
  });
  implementGlobal(vm, u"current_date", [](VM&, Object*, int, const Value*) {
    std::time_t t = std::time(nullptr);
    std::tm* tm = std::localtime(&t);
    return Value::integer(mkdate(tm->tm_mday, tm->tm_mon + 1, tm->tm_year + 1900));
  });
  implementGlobal(vm, u"current_time", [](VM&, Object*, int, const Value*) {
    std::time_t t = std::time(nullptr);
    std::tm* tm = std::localtime(&t);
    return Value::integer(((tm->tm_hour * 60 + tm->tm_min) * 60 + tm->tm_sec) * 1000);
  });

  // ---- strings ----
  implementGlobal(vm, u"expandPattern", [](VM& v, Object*, int argc, const Value* a) {
    return str(formatPattern(v, deref(a[0]).toString(), a + 1, argc - 1));
  });
  implementGlobal(vm, u"escapePattern", [](VM&, Object*, int, const Value* a) {
    std::u16string s, in = deref(a[0]).toString();
    for (char16_t c : in) {
      if (c == '%') s += u'%';
      s += c;
    }
    return str(s);
  });
  implementGlobal(vm, u"substr", [](VM&, Object*, int argc, const Value* a) {
    std::u16string s = deref(a[0]).toString();
    int32_t from = std::max(0, deref(a[1]).toInt());
    if (size_t(from) >= s.size()) return str(u"");
    // std::wstring::assign(str, pos, count): a negative count becomes a huge unsigned one, i.e. "to the end"
    int32_t n = argc > 2 ? deref(a[2]).toInt() : -1;
    size_t len = n < 0 ? std::u16string::npos : size_t(n);
    return str(s.substr(size_t(from), len));
  });
  implementGlobal(vm, u"findstr", [](VM&, Object*, int, const Value* a) {
    size_t k = deref(a[0]).toString().find(deref(a[1]).toString());
    return Value::integer(k == std::u16string::npos ? -1 : int32_t(k));
  });
  // trimFirstLines(text, n) (0x1000bb70, L:0x0824d950): keeps the last n lines. Scans back from the second-last
  // character; at the n-th '\n' everything up to and including it is cut. Used by the debug console.
  implementGlobal(vm, u"trimFirstLines", [](VM&, Object*, int, const Value* a) {
    std::u16string s = deref(a[0]).toString();
    int32_t n = deref(a[1]).toInt(), seen = 0;
    for (int64_t i = int64_t(s.size()) - 2; i >= 0; --i)
      if (s[size_t(i)] == u'\n' && ++seen == n) {
        s.erase(0, size_t(i) + 1);
        break;
      }
    return str(s);
  });
  implementGlobal(vm, u"replacestr", [](VM&, Object*, int, const Value* a) {
    std::u16string s = deref(a[0]).toString(), from = deref(a[1]).toString(), to = deref(a[2]).toString();
    if (from.empty()) return str(s);
    for (size_t k = 0; (k = s.find(from, k)) != std::u16string::npos; k += to.size()) s.replace(k, from.size(), to);
    return str(s);
  });
  // the engine's own case tables (0x1000bc90 / 0x1000beb0): ASCII and Latin-1 letters, the cp1252 code units of
  // Š Œ Ž Ÿ (0x8A-0x9F, as Latin-1-decoded files contain them), Cyrillic, and the paired Latin Extended-A letters
  // pairs with the capital on the even code point (A) or on the odd one (B)
  auto pairA = [](char16_t c) { return (c >= 0x100 && c <= 0x137) || (c >= 0x14A && c <= 0x177) || (c >= 0x460 && c <= 0x523); };
  auto pairB = [](char16_t c) { return (c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E); };
  auto pairedUpper = [pairA, pairB](char16_t c) { return (pairA(c) && !(c & 1)) || (pairB(c) && (c & 1)); };
  auto pairedLower = [pairA, pairB](char16_t c) { return (pairA(c) && (c & 1)) || (pairB(c) && !(c & 1)); };
  implementGlobal(vm, u"lowercase", [pairedUpper](VM&, Object*, int, const Value* a) {
    std::u16string s = deref(a[0]).toString();
    for (auto& c : s) {
      if ((c >= 0x41 && c <= 0x5A) || (c >= 0xC0 && c <= 0xD6) || (c >= 0xD8 && c <= 0xDE) || (c >= 0x410 && c <= 0x42F)) c += 0x20;
      else if (c == 0x8A || c == 0x8C || c == 0x8E) c += 0x10;
      else if (c == 0x9F) c = 0xFF;
      else if (c >= 0x400 && c <= 0x40F) c += 0x50;
      else if (pairedUpper(c)) c += 1;
    }
    return str(s);
  });
  implementGlobal(vm, u"uppercase", [pairedLower](VM&, Object*, int, const Value* a) {
    std::u16string s = deref(a[0]).toString();
    for (auto& c : s) {
      if ((c >= 0x61 && c <= 0x7A) || (c >= 0xE0 && c <= 0xF6) || (c >= 0xF8 && c <= 0xFE) || (c >= 0x430 && c <= 0x44F)) c -= 0x20;
      else if (c == 0x9A || c == 0x9C || c == 0x9E) c -= 0x10;
      else if (c == 0xFF) c = 0x9F;
      else if (c >= 0x450 && c <= 0x45F) c -= 0x50;
      else if (pairedLower(c)) c -= 1;
    }
    return str(s);
  });
  implementGlobal(vm, u"getenv", [](VM&, Object*, int, const Value* a) {
    const char* e = std::getenv(u8(deref(a[0]).toString()).c_str());
    return str(e ? w(e) : u"");
  });

  // ---- arrays and maps ----
  implementGlobal(vm, u"array_erase", [](VM&, Object*, int, const Value* a) {
    if (ArrayObj* arr = deref(a[0]).array()) {
      int32_t i = deref(a[1]).toInt();
      if (i >= 0 && size_t(i) < arr->items.size()) arr->items.erase(arr->items.begin() + i);
    }
    return Value();
  });
  // array_splice(array, start, count) (0x1000b560 → gsArrayVariant::splice L:0x08272520): removes count elements at
  // start in place (a negative start shortens count; count clamped to the end) and returns the same array
  implementGlobal(vm, u"array_splice", [](VM& v, Object*, int, const Value* a) {
    Value arrV = deref(a[0]);
    ArrayObj* arr = arrV.array();
    if (!arr) v.error(kErrInvalidArgs, "Invalid arguments!");
    int64_t from = deref(a[1]).toInt(), n = deref(a[2]).toInt();
    if (from < 0) {
      n += from;
      from = 0;
    }
    n = std::min<int64_t>(n, int64_t(arr->items.size()) - from);
    if (n > 0) arr->items.erase(arr->items.begin() + from, arr->items.begin() + from + n);
    return arrV;
  });
  implementGlobal(vm, u"array_shuffle", [](VM&, Object*, int, const Value* a) {
    if (ArrayObj* arr = deref(a[0]).array())
      for (size_t i = arr->items.size(); i > 1; --i) std::swap(arr->items[i - 1], arr->items[size_t(rand31()) % i]);
    return Value();
  });
  implementGlobal(vm, u"map_erase", [](VM&, Object*, int, const Value* a) {
    if (MapObj* m = deref(a[0]).map()) m->erase(deref(a[1]));
    return Value();
  });

  // ---- files ----
  implementGlobal(vm, u"getMyDocumentsFolder", [](VM&, Object*, int, const Value*) {
    auto p = host().documents.wstring();
    std::u16string s(p.begin(), p.end());
    for (auto& c : s) if (c == '\\') c = '/';
    return str(s);
  });
  // init_userdata(root): the port puts the original's <documents>/Cities In Motion at <documents>/OpenCiM and reads
  // the original folder only (engine/user_folders.h)
  implementGlobal(vm, u"init_userdata", [](VM& v, Object*, int, const Value* a) {
    std::u16string root = deref(a[0]).toString();
    v.vfs().initUserdata(redirectUserdata(v.vfs(), fs::path(std::wstring(root.begin(), root.end()))));
    return Value();
  });
  implementGlobal(vm, u"updateFileSystem", [](VM& v, Object*, int, const Value*) {
    v.vfs().rescan();
    return Value();
  });
  implementGlobal(vm, u"file_exists", [](VM& v, Object*, int, const Value* a) {
    std::u16string p = deref(a[0]).toString();
    if ((p.size() > 1 && p[1] == ':') || (!p.empty() && p[0] == '/')) return Value::boolean(fs::exists(fs::path(std::wstring(p.begin(), p.end()))));
    return Value::boolean(v.vfs().exists(u8(p)));
  });
  implementGlobal(vm, u"copy_file", [](VM& v, Object*, int, const Value* a) {
    std::vector<uint8_t> data;
    std::string src = u8(deref(a[0]).toString());
    if (!v.vfs().read(src, data)) v.error("File \"" + src + "\" not found!");
    v.vfs().write(u8(deref(a[1]).toString()), data);
    return Value();
  });
  // disk changes outside Vfs::write go through writableDiskPath too: only below userdata/ (the port's user folder)
  implementGlobal(vm, u"delete_file", [](VM& v, Object*, int, const Value* a) {
    std::error_code ec;
    fs::path p;
    if (v.vfs().writableDiskPath(u8(deref(a[0]).toString()), p)) fs::remove(p, ec);
    return Value();
  });
  implementGlobal(vm, u"mkdir", [](VM& v, Object*, int, const Value* a) {
    std::error_code ec;
    fs::path p;
    if (v.vfs().writableDiskPath(u8(deref(a[0]).toString()), p)) fs::create_directory(p, ec);
    return Value();
  });
  // writePackage(path, files) (L:0x081058b0): the files (usually memory/…) bundled under their names into a GS10
  // archive at path (userdata only), which the file system then shows as the folder path minus ".gs"
  implementGlobal(vm, u"writePackage", [](VM& v, Object*, int, const Value* a) {
    std::vector<std::pair<std::string, std::vector<uint8_t>>> files;
    if (ArrayObj* arr = deref(a[1]).array())
      for (auto& x : arr->items) {
        std::string src = u8(deref(x).toString());
        std::vector<uint8_t> data;
        if (!v.vfs().read(src, data)) v.error("File \"" + src + "\" not found!");
        files.emplace_back(src.substr(src.find_last_of('/') + 1), std::move(data));
      }
    std::string path = u8(deref(a[0]).toString());
    if (!v.vfs().write(path, buildPackage(files))) LOG_ERROR("writePackage: cannot write %s", path.c_str());
    v.vfs().rescan();  // the engine adds the package's entries to the file table at once
    return Value();
  });
  implementGlobal(vm, u"list_files", [](VM& v, Object*, int argc, const Value* a) {
    std::vector<Value> out;
    for (auto& n : v.vfs().listFiles(argc > 0 ? u8(deref(a[0]).toString()) : "", argc > 1 ? u8(deref(a[1]).toString()) : ""))
      out.push_back(str(w(n)));
    return array(std::move(out));
  });
  implementGlobal(vm, u"list_dirs", [](VM& v, Object*, int argc, const Value* a) {
    std::vector<Value> out;
    for (auto& n : v.vfs().listDirs(argc > 0 ? u8(deref(a[0]).toString()) : "")) out.push_back(str(w(n)));
    return array(std::move(out));
  });
  implementGlobal(vm, u"extractFileName", [](VM&, Object*, int, const Value* a) { return str(extractName(deref(a[0]).toString())); });
  implementGlobal(vm, u"extractFilePath", [](VM&, Object*, int, const Value* a) {
    std::u16string p = deref(a[0]).toString();
    size_t k = p.find_last_of(u"/\\");
    return str(k == std::u16string::npos ? u"" : p.substr(0, k + 1));
  });
  implementGlobal(vm, u"extractFileDir", [](VM&, Object*, int, const Value* a) {
    std::u16string p = deref(a[0]).toString();
    size_t k = p.find_last_of(u"/\\");
    return str(k == std::u16string::npos ? u"" : p.substr(0, k));
  });
  implementGlobal(vm, u"extractFileExt", [](VM&, Object*, int, const Value* a) {
    std::u16string n = extractName(deref(a[0]).toString());
    size_t k = n.rfind(u'.');
    return str(k == std::u16string::npos ? u"" : n.substr(k));
  });
  implementGlobal(vm, u"changeFileExt", [](VM&, Object*, int, const Value* a) {
    std::u16string p = deref(a[0]).toString(), ext = deref(a[1]).toString();
    size_t sep = p.find_last_of(u"/\\");
    size_t dot = p.rfind(u'.');
    if (dot != std::u16string::npos && (sep == std::u16string::npos || dot > sep)) p = p.substr(0, dot);
    return str(p + ext);
  });
  implementGlobal(vm, u"fappend", [](VM& v, Object*, int, const Value* a) {
    fs::path p;
    if (!v.vfs().writableDiskPath(u8(deref(a[0]).toString()), p)) return Value();
    FILE* f = _wfopen(p.wstring().c_str(), L"a");
    if (!f) v.error("Cannot open file \"" + u8(deref(a[0]).toString()) + "\"");
    std::u16string s = deref(a[1]).toString();
    for (char16_t c : s) std::fputc(int(uint8_t(c)), f);
    std::fclose(f);
    return Value();
  });
  implementGlobal(vm, u"freset", [](VM& v, Object*, int, const Value* a) {
    fs::path p;
    if (!v.vfs().writableDiskPath(u8(deref(a[0]).toString()), p)) return Value();
    if (FILE* f = _wfopen(p.wstring().c_str(), L"w")) std::fclose(f);
    return Value();
  });
  implementGlobal(vm, u"read_file", [](VM& v, Object*, int, const Value* a) {
    std::vector<uint8_t> data;
    if (!v.vfs().read(u8(deref(a[0]).toString()), data)) return Value();
    return str(decodeTextFile(data));
  });
  implementGlobal(vm, u"validatePackage", [](VM& v, Object*, int, const Value* a) {
    return Value::boolean(isValidPackage(v.vfs().diskPathFor(u8(deref(a[0]).toString()))));
  });
  implementGlobal(vm, u"quit", [](VM&, Object*, int, const Value*) {
    host().quit = true;
    return Value();
  });

  // ---- gsAchievements: awardAchievement does nothing in the original (L:0x080d9150); awardSteamAchievement
  // (L:0x080d98b0) only submits to Steam, which the port has not. The scenarios' own trophies live in state.gs. ----
  implement(vm, "gsAchievements", u"awardAchievement", [](VM&, Object*, int, const Value*) { return Value(); });
  implement(vm, "gsAchievements", u"awardSteamAchievement", [](VM&, Object*, int, const Value*) { return Value(); });

  // ---- gsScript / gsObjectVariant ----
  implement(vm, "gsScript", u"getVariable", [](VM&, Object* self, int, const Value* a) {
    Value* v = self->findVar(deref(a[0]).toString());
    return v ? *v : Value();
  });
  implement(vm, "gsScript", u"setVariable", [](VM&, Object* self, int, const Value* a) {
    self->var(deref(a[0]).toString()) = deref(a[1]);
    return Value();
  });
  // getFunction(name): the script's own function, valueless when there is none (0x10017470, batch-C.md)
  implement(vm, "gsScript", u"getFunction", [](VM& v, Object* self, int, const Value* a) {
    try {
      Value f = v.getMember(Value::heap(VT::Object, self), deref(a[0]).toString());
      return f.func() ? f : Value();
    } catch (const ScriptError&) {
      return Value();
    }
  });
  implement(vm, "gsScript", u"listVariables", [](VM&, Object* self, int, const Value*) {
    std::vector<std::u16string> names;
    for (auto& [k, val] : self->vars) names.push_back(k);
    std::sort(names.begin(), names.end());
    std::vector<Value> out;
    for (auto& n : names) out.push_back(str(n));
    return array(std::move(out));
  });
  implement(vm, "gsScript", u"mergeVariables", [](VM&, Object* self, int, const Value* a) {
    if (Object* o = deref(a[0]).object())
      for (auto& [k, val] : o->vars) self->var(k) = val;
    return Value();
  });
  implement(vm, "gsScript", u"exportVariables", [](VM& v, Object* self, int argc, const Value* a) {
    if (argc > 0) exportVariables(v, self, self->scriptPath, argc, a);
    return Value();
  });
  implement(vm, "gsScript", u"exportVariablesTo", [](VM& v, Object* self, int argc, const Value* a) {
    exportVariables(v, self, deref(a[0]).toString(), argc - 1, a + 1);
    return Value();
  });
  if (ClassInfo* s = vm.findClass("gsScript")) {
    if (auto it = s->fields.find(u"scriptFilename"); it != s->fields.end()) {
      // writable (saveScenario / saveCampaign point a script at the file they exported it to)
      it->second.get = [](VM&, Object* o) { return Value::string(o->scriptPath); };
      it->second.set = [](VM& v, Object* o, const Value& val) { o->scriptPath = v.toStr(val); };
    }
  }
  implement(vm, "gsObjectVariant", u"className", [](VM&, Object* self, int, const Value*) {
    return Value::string(self->cls->name.c_str());
  });
  implement(vm, "gsObjectVariant", u"hasMember", [](VM& v, Object* self, int, const Value* a) {
    std::u16string n = deref(a[0]).toString();
    if (!n.empty() && n[0] == '$') return Value::boolean(self->findVar(n.substr(1)) != nullptr);
    return Value::boolean(self->cls->findField(n) || self->cls->findMethod(n) ||
                          (self->code && self->code->functionIndex.count(n)));
  });
  implement(vm, "gsObjectVariant", u"getMember", [](VM& v, Object* self, int, const Value* a) {
    return v.getMember(Value::heap(VT::Object, self), deref(a[0]).toString());
  });
  if (ClassInfo* o = vm.findClass("gsObjectVariant")) {
    if (auto it = o->fields.find(u"destroyed"); it != o->fields.end()) it->second.get = [](VM&, Object* ob) { return Value::boolean(ob->destroyed); };
  }

  // ---- display modes (gsDisplayModeVariant: width, height, bits, freq) ----
  auto displayMode = [](VM& v, int w, int h, int bits, int freq) {
    Ref<Object> m = v.newObject(v.findClass("gsDisplayModeVariant"), nullptr);
    m->fieldValues[u"width"] = Value::integer(w);
    m->fieldValues[u"height"] = Value::integer(h);
    m->fieldValues[u"bits"] = Value::integer(bits);
    m->fieldValues[u"freq"] = Value::integer(freq);
    return Value::heap(VT::Object, m.get());
  };
  implementGlobal(vm, u"display_modes", [displayMode](VM& v, Object*, int, const Value*) {
    std::vector<Value> out;
    for (auto& m : platformDisplayModes()) out.push_back(displayMode(v, m.width, m.height, m.bits, m.freq));
    return array(std::move(out));
  });
  implementGlobal(vm, u"current_display_mode", [displayMode](VM& v, Object*, int, const Value*) {
    auto m = platformCurrentDisplayMode();
    return displayMode(v, m.width, m.height, m.bits, m.freq);
  });

  // ---- window: the window owns its video device object (gsWindow.video) ----
  implementGlobal(vm, u"new_window", [](VM& v, Object*, int argc, const Value* a) {
    Object* parent = argc > 1 ? deref(a[1]).object() : nullptr;
    Ref<Object> win = v.newObject(v.findClass("gsWindow_OGL"), parent);
    Ref<Object> video = v.newObject(v.findClass("gsVideo_OGL"), win.get());
    win->fieldValues[u"video"] = Value::heap(VT::Object, video.get());
    return Value::heap(VT::Object, win.get());
  });

  // ---- gsModule tree ----
  implement(vm, "gsModule", u"children", [](VM&, Object* self, int, const Value*) {
    std::vector<Value> out;
    for (auto& c : self->children) out.push_back(Value::heap(VT::Object, c.get()));
    return array(std::move(out));
  });
  implement(vm, "gsModule", u"getChildCount", [](VM&, Object* self, int, const Value*) { return Value::integer(int32_t(self->children.size())); });
  implement(vm, "gsModule", u"clear", [](VM& v, Object* self, int, const Value*) {
    auto kids = self->children;
    for (auto& c : kids) v.destroy(c.get());
    return Value();
  });
  implement(vm, "gsModule", u"show", [](VM& v, Object* self, int, const Value*) {
    v.setMember(Value::heap(VT::Object, self), u"visible", Value::boolean(true));
    return Value();
  });
  implement(vm, "gsModule", u"hide", [](VM& v, Object* self, int, const Value*) {
    v.setMember(Value::heap(VT::Object, self), u"visible", Value::boolean(false));
    return Value();
  });
}

}  // namespace regs::engine
