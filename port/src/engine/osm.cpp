// OpenStreetMap import for the map editor (see osm.h). Sources, all fetched with the port's User-Agent and cached:
// * view tiles: Esri World Imagery (server.arcgisonline.com, attribution shown in the window) and the OpenStreetMap
//   standard tiles (tile.openstreetmap.org, at most two downloads at a time as its tile policy asks);
// * search: Nominatim (one request per search, on Enter or the button, never while typing);
// * elevation: AWS Terrain Tiles, Terrarium encoding (height = R·256 + G + B/256 − 32768 m; includes bathymetry);
// * map data: the Overpass API (one query for the area).
// The map's cells are laid over the Web Mercator plane around the rectangle's centre: cell size 3 m times the scale
// factor at that latitude, x to the east, z to the south (the minimap's top is north).
#include "engine/osm.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/display.h"
#include "engine/grid.h"
#include "engine/grid_edit.h"
#include "engine/gui.h"
#include "engine/host.h"
#include "engine/interaction.h"
#include "engine/platform.h"
#include "engine/timeline.h"
#include "engine/tools.h"
#include "render/renderer2d.h"

namespace regs::engine {

using namespace regs::script;

GroundOverlay& groundOverlay() {
  static GroundOverlay o;
  return o;
}

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEarth = 40075016.686;  // equator length in metres (Web Mercator)
constexpr int kNoY = -0x8000;

double now() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------------------------------------------
// a small JSON reader (Nominatim and Overpass answers)

struct Json {
  enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
  double n = 0;
  bool b = false;
  std::string s;
  std::vector<Json> a;
  std::vector<std::pair<std::string, Json>> o;
  const Json* get(const char* k) const {
    if (t != Obj) return nullptr;
    for (auto& [key, v] : o)
      if (key == k) return &v;
    return nullptr;
  }
  std::string str(const char* k) const {
    const Json* v = get(k);
    if (!v) return {};
    if (v->t == Str) return v->s;
    if (v->t == Num) return std::to_string(v->n);
    return {};
  }
  double num(const char* k, double d = 0) const {
    const Json* v = get(k);
    if (!v) return d;
    if (v->t == Num) return v->n;
    if (v->t == Str) return std::atof(v->s.c_str());
    return d;
  }
};

struct JsonParser {
  const char* p;
  const char* e;
  bool ok = true;
  void ws() {
    while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
  }
  static void utf8(std::string& out, uint32_t c) {
    if (c < 0x80) out += char(c);
    else if (c < 0x800) out += char(0xC0 | (c >> 6)), out += char(0x80 | (c & 0x3F));
    else if (c < 0x10000) out += char(0xE0 | (c >> 12)), out += char(0x80 | ((c >> 6) & 0x3F)), out += char(0x80 | (c & 0x3F));
    else
      out += char(0xF0 | (c >> 18)), out += char(0x80 | ((c >> 12) & 0x3F)), out += char(0x80 | ((c >> 6) & 0x3F)),
          out += char(0x80 | (c & 0x3F));
  }
  uint32_t hex4() {
    uint32_t v = 0;
    for (int i = 0; i < 4 && p < e; ++i, ++p) {
      char c = *p;
      v <<= 4;
      if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
      else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
    }
    return v;
  }
  std::string string() {
    std::string out;
    ++p;  // "
    while (p < e && *p != '"') {
      if (*p == '\\' && p + 1 < e) {
        ++p;
        char c = *p++;
        switch (c) {
          case 'n': out += '\n'; break;
          case 't': out += '\t'; break;
          case 'r': out += '\r'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'u': {
            uint32_t u = hex4();
            if (u >= 0xD800 && u < 0xDC00 && p + 1 < e && p[0] == '\\' && p[1] == 'u') {
              p += 2;
              uint32_t lo = hex4();
              u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
            }
            utf8(out, u);
            break;
          }
          default: out += c;
        }
      } else {
        out += *p++;
      }
    }
    if (p < e) ++p;
    return out;
  }
  Json value(int depth = 0) {
    Json j;
    ws();
    if (p >= e || depth > 200) {
      ok = false;
      return j;
    }
    char c = *p;
    if (c == '{') {
      j.t = Json::Obj;
      ++p;
      ws();
      if (p < e && *p == '}') {
        ++p;
        return j;
      }
      while (p < e) {
        ws();
        if (p >= e || *p != '"') {
          ok = false;
          return j;
        }
        std::string k = string();
        ws();
        if (p < e && *p == ':') ++p;
        j.o.emplace_back(std::move(k), value(depth + 1));
        ws();
        if (p < e && *p == ',') {
          ++p;
          continue;
        }
        if (p < e && *p == '}') ++p;
        break;
      }
    } else if (c == '[') {
      j.t = Json::Arr;
      ++p;
      ws();
      if (p < e && *p == ']') {
        ++p;
        return j;
      }
      while (p < e) {
        j.a.push_back(value(depth + 1));
        ws();
        if (p < e && *p == ',') {
          ++p;
          continue;
        }
        if (p < e && *p == ']') ++p;
        break;
      }
    } else if (c == '"') {
      j.t = Json::Str;
      j.s = string();
    } else if (c == 't' || c == 'f') {
      j.t = Json::Bool;
      j.b = c == 't';
      p += j.b ? 4 : 5;
    } else if (c == 'n') {
      p += 4;
    } else {
      j.t = Json::Num;
      char* end = nullptr;
      j.n = std::strtod(p, &end);
      if (end == p) {
        ok = false;
        ++p;
      } else {
        p = end;
      }
    }
    return j;
  }
};

bool parseJson(const std::vector<uint8_t>& bytes, Json& out) {
  JsonParser jp{reinterpret_cast<const char*>(bytes.data()), reinterpret_cast<const char*>(bytes.data()) + bytes.size()};
  out = jp.value();
  return jp.ok;
}

std::string urlEncode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : s) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' ||
        c == '~')
      out += char(c);
    else
      out += '%', out += hex[c >> 4], out += hex[c & 15];
  }
  return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Web Mercator: (lon, lat) ↔ (mx, my) in [0, 1], my growing to the south

double lonToMx(double lon) { return (lon + 180.0) / 360.0; }
double latToMy(double lat) {
  lat = std::clamp(lat, -85.05112878, 85.05112878) * kPi / 180.0;
  return (1.0 - std::log(std::tan(lat) + 1.0 / std::cos(lat)) / kPi) * 0.5;
}
double mxToLon(double mx) { return mx * 360.0 - 180.0; }
double myToLat(double my) { return std::atan(std::sinh(kPi * (1.0 - 2.0 * my))) * 180.0 / kPi; }
// metres per Mercator unit at the latitude of my
double metresPerUnit(double my) { return kEarth * std::cos(myToLat(my) * kPi / 180.0); }

// ---------------------------------------------------------------------------------------------------------------
// tiles: layers 0 OpenStreetMap, 1 Esri World Imagery, 2 Terrarium elevation

struct TileKey {
  int layer, z, x, y;
  bool operator<(const TileKey& o) const {
    if (layer != o.layer) return layer < o.layer;
    if (z != o.z) return z < o.z;
    if (x != o.x) return x < o.x;
    return y < o.y;
  }
};

std::string tileUrl(const TileKey& k) {
  char buf[256];
  switch (k.layer) {
    case 0: std::snprintf(buf, sizeof buf, "https://tile.openstreetmap.org/%d/%d/%d.png", k.z, k.x, k.y); break;
    case 1:
      std::snprintf(buf, sizeof buf, "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/%d/%d/%d",
                    k.z, k.y, k.x);
      break;
    default:
      std::snprintf(buf, sizeof buf, "https://s3.amazonaws.com/elevation-tiles-prod/terrarium/%d/%d/%d.png", k.z, k.x, k.y);
  }
  return buf;
}

std::filesystem::path cacheRoot() {
  std::filesystem::path base = host().userFolder.empty() ? host().documents / "OpenCiM" : host().userFolder;
  return base / "cache" / "osm";
}

std::filesystem::path tilePath(const TileKey& k) {
  static const char* names[] = {"osm", "esri", "terrarium"};
  return cacheRoot() / names[k.layer] / std::to_string(k.z) / std::to_string(k.x) / (std::to_string(k.y) + ".img");
}

bool readFile(const std::filesystem::path& p, std::vector<uint8_t>& out) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return !out.empty();
}
void writeFile(const std::filesystem::path& p, const std::vector<uint8_t>& data) {
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream f(p, std::ios::binary);
  f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
}

// one tile from the disk cache or the server (blocking; worker threads); empty on failure
bool fetchTile(const TileKey& k, std::vector<uint8_t>& bgra) {
  std::vector<uint8_t> file;
  std::filesystem::path p = tilePath(k);
  int w = 0, h = 0;
  if (readFile(p, file) && platformDecodeImage(file, w, h, bgra) && w == 256 && h == 256) return true;
  std::string err;
  int status = platformHttpRequest(tileUrl(k), nullptr, file, &err);
  if (status != 200 || !platformDecodeImage(file, w, h, bgra) || w != 256 || h != 256) {
    if (status != 404) LOG_WARN("osm: tile %s: status %d %s", tileUrl(k).c_str(), status, err.c_str());
    bgra.clear();
    return false;
  }
  writeFile(p, file);
  return true;
}

struct Tile {
  int state = 0;  // 0 queued, 1 loaded, 2 failed
  std::vector<uint8_t> px;
  double used = 0, failedAt = 0;
};

struct TileStore {
  std::mutex mx;
  std::condition_variable cv;
  std::map<TileKey, Tile> tiles;
  std::deque<TileKey> queue;
  std::atomic<uint64_t> version{0};
  bool started = false;

  void start() {
    if (started) return;
    started = true;
    for (int i = 0; i < 2; ++i) std::thread([this] { work(); }).detach();
  }
  void work() {
    for (;;) {
      TileKey k{};
      {
        std::unique_lock<std::mutex> lk(mx);
        cv.wait(lk, [&] { return !queue.empty(); });
        k = queue.back();  // the newest request first (the tiles on screen now)
        queue.pop_back();
        auto it = tiles.find(k);
        if (it == tiles.end() || it->second.state != 0) continue;
        if (now() - it->second.used > 3.0) {  // scrolled away meanwhile
          tiles.erase(it);
          continue;
        }
      }
      std::vector<uint8_t> px;
      bool ok = fetchTile(k, px);
      {
        std::lock_guard<std::mutex> lk(mx);
        Tile& t = tiles[k];
        t.state = ok ? 1 : 2;
        t.px = std::move(px);
        t.failedAt = now();
        // keep memory bounded: drop the least recently used loaded tiles over 600
        if (tiles.size() > 700) {
          std::vector<std::pair<double, TileKey>> by;
          for (auto& [key, tile] : tiles)
            if (tile.state != 0) by.push_back({tile.used, key});
          std::sort(by.begin(), by.end(), [](auto& a, auto& b) { return a.first < b.first; });
          for (size_t i = 0; i + 600 < by.size(); ++i) tiles.erase(by[i].second);
        }
      }
      ++version;
    }
  }
  // the tile's pixels when loaded (the caller holds mx); queues it otherwise
  const std::vector<uint8_t>* get(const TileKey& k, bool request, double t) {
    auto it = tiles.find(k);
    if (it == tiles.end()) {
      if (!request) return nullptr;
      Tile& n = tiles[k];
      n.used = t;
      queue.push_back(k);
      cv.notify_one();
      return nullptr;
    }
    it->second.used = t;
    if (it->second.state == 1) return &it->second.px;
    if (it->second.state == 2 && request && t - it->second.failedAt > 20.0) {  // try again later
      it->second.state = 0;
      queue.push_back(k);
      cv.notify_one();
    }
    return nullptr;
  }
};

TileStore& store() {
  static TileStore s;
  s.start();
  return s;
}

// ---------------------------------------------------------------------------------------------------------------
// the view and the rectangle

struct View {
  double mx = 0.5, my = 0.5;  // the centre
  int z = 2;
  int layer = 1;
  bool hasArea = false;
  double amx = 0.5, amy = 0.5;  // the rectangle's centre
  int cellsW = 256, cellsL = 256;
  double scale = 1.0;
  int lastW = 0, lastH = 0;
  bool placed = false;  // the player (or a start view) has put the view somewhere
};
View g_view;

double worldSize(int z) { return 256.0 * double(1 << z); }

// the rectangle's size in Mercator units
void areaSize(double& w, double& l) {
  double m = metresPerUnit(g_view.amy);
  w = g_view.cellsW * 3.0 * g_view.scale / m;
  l = g_view.cellsL * 3.0 * g_view.scale / m;
}

void compose(std::vector<uint8_t>& out, int W, int H, bool& loading) {
  out.assign(size_t(W) * size_t(H) * 4, 0);
  for (size_t i = 0; i < out.size(); i += 4) out[i] = 0x30, out[i + 1] = 0x2a, out[i + 2] = 0x26, out[i + 3] = 0xFF;
  const int z = g_view.z;
  const double S = worldSize(z);
  const double left = g_view.mx * S - W * 0.5, top = g_view.my * S - H * 0.5;
  const int n = 1 << z;
  TileStore& ts = store();
  double t = now();
  loading = false;
  std::lock_guard<std::mutex> lk(ts.mx);
  int tx0 = int(std::floor(left / 256.0)), tx1 = int(std::floor((left + W - 1) / 256.0));
  int ty0 = std::max(0, int(std::floor(top / 256.0))), ty1 = std::min(n - 1, int(std::floor((top + H - 1) / 256.0)));
  for (int ty = ty0; ty <= ty1; ++ty)
    for (int tx = tx0; tx <= tx1; ++tx) {
      int wx = ((tx % n) + n) % n;
      const std::vector<uint8_t>* px = ts.get({g_view.layer, z, wx, ty}, true, t);
      int d = 0, sub = 256, ox = 0, oy = 0;
      if (!px) {
        loading = true;
        for (d = 1; d <= 6 && d <= z && !px; ++d) {  // a coarser tile meanwhile
          px = ts.get({g_view.layer, z - d, wx >> d, ty >> d}, false, t);
          if (px) {
            sub = 256 >> d;
            ox = (wx & ((1 << d) - 1)) * sub;
            oy = (ty & ((1 << d) - 1)) * sub;
            break;
          }
        }
      }
      if (!px) continue;
      int sx0 = int(std::floor(tx * 256.0 - left)), sy0 = int(std::floor(ty * 256.0 - top));
      for (int yy = std::max(0, sy0); yy < std::min(H, sy0 + 256); ++yy) {
        int ty2 = oy + ((yy - sy0) * sub >> 8);
        const uint8_t* src = px->data() + size_t(ty2) * 1024;
        uint8_t* dst = out.data() + size_t(yy) * size_t(W) * 4;
        for (int xx = std::max(0, sx0); xx < std::min(W, sx0 + 256); ++xx) {
          int tx2 = ox + ((xx - sx0) * sub >> 8);
          std::memcpy(dst + size_t(xx) * 4, src + size_t(tx2) * 4, 3);
        }
      }
    }
  if (!g_view.hasArea) return;
  double aw, al;
  areaSize(aw, al);
  double rx0 = (g_view.amx - aw * 0.5) * S - left, ry0 = (g_view.amy - al * 0.5) * S - top;
  double rx1 = (g_view.amx + aw * 0.5) * S - left, ry1 = (g_view.amy + al * 0.5) * S - top;
  int x0 = int(std::lround(rx0)), y0 = int(std::lround(ry0)), x1 = int(std::lround(rx1)), y1 = int(std::lround(ry1));
  if (x1 - x0 < 6) x0 = (x0 + x1) / 2 - 3, x1 = x0 + 6;
  if (y1 - y0 < 6) y0 = (y0 + y1) / 2 - 3, y1 = y0 + 6;
  for (int yy = 0; yy < H; ++yy) {
    uint8_t* row = out.data() + size_t(yy) * size_t(W) * 4;
    for (int xx = 0; xx < W; ++xx) {
      bool inside = xx >= x0 && xx < x1 && yy >= y0 && yy < y1;
      bool border = inside && (xx < x0 + 2 || xx >= x1 - 2 || yy < y0 + 2 || yy >= y1 - 2);
      uint8_t* p = row + size_t(xx) * 4;
      if (border) p[0] = 0x60, p[1] = 0xC8, p[2] = 0xE8;  // the game's gold (0xE8C860)
      else if (!inside) p[0] = uint8_t(p[0] * 0.55), p[1] = uint8_t(p[1] * 0.55), p[2] = uint8_t(p[2] * 0.55);
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------
// background jobs: the search and the import downloads

struct SearchJob {
  std::mutex mx;
  int state = 0;  // 0 idle, 1 running, 2 done, 3 failed
  int serial = 0;
  struct Hit {
    std::string name;
    double lat, lon;
    int zoom;
  };
  std::vector<Hit> hits;
};
SearchJob g_search;

int zoomForBox(double s, double n, double w, double e, int px) {
  double dx = std::abs(lonToMx(e) - lonToMx(w)), dy = std::abs(latToMy(s) - latToMy(n));
  double d = std::max({dx, dy, 1e-9});
  int z = int(std::floor(std::log2(px / (256.0 * d))));
  return std::clamp(z, 2, 17);
}

void startSearch(const std::string& text, const std::string& lang) {
  int serial;
  {
    std::lock_guard<std::mutex> lk(g_search.mx);
    serial = ++g_search.serial;
    g_search.state = 1;
    g_search.hits.clear();
  }
  std::thread([text, lang, serial] {
    std::string url = "https://nominatim.openstreetmap.org/search?format=jsonv2&limit=8&accept-language=" + urlEncode(lang) +
                      "&q=" + urlEncode(text);
    std::vector<uint8_t> body;
    std::string err;
    int status = platformHttpRequest(url, nullptr, body, &err);
    Json j;
    std::vector<SearchJob::Hit> hits;
    bool ok = status == 200 && parseJson(body, j) && j.t == Json::Arr;
    if (ok)
      for (const Json& r : j.a) {
        SearchJob::Hit h;
        h.name = r.str("display_name");
        h.lat = r.num("lat");
        h.lon = r.num("lon");
        h.zoom = 14;
        if (h.name.size() > 75) {  // the list shows one line per place
          size_t cut = 75;
          while (cut > 0 && (uint8_t(h.name[cut]) & 0xC0) == 0x80) --cut;
          h.name = h.name.substr(0, cut) + "...";
        }
        if (const Json* bb = r.get("boundingbox"); bb && bb->t == Json::Arr && bb->a.size() == 4) {
          auto num = [&](int i) { return bb->a[size_t(i)].t == Json::Str ? std::atof(bb->a[size_t(i)].s.c_str()) : bb->a[size_t(i)].n; };
          h.zoom = std::min(14, zoomForBox(num(0), num(1), num(2), num(3), 500));
        }
        hits.push_back(std::move(h));
      }
    else
      LOG_WARN("osm: search failed: status %d %s", status, err.c_str());
    std::lock_guard<std::mutex> lk(g_search.mx);
    if (serial != g_search.serial) return;
    g_search.hits = std::move(hits);
    g_search.state = ok ? 2 : 3;
  }).detach();
}

// what the import found, in the map's cell coordinates
struct Way {
  std::vector<std::pair<double, double>> pts;
};
struct RoadIn {
  int kind = 0;  // 0 small road, 1 large road (multi-lane), 2 alley, 3 avenue (trees in the median), 4 express road
  bool bridge = false, oneway = false, treeLined = false, dropped = false;
  int cls = 0;   // 0 minor, 1 tertiary, 2 secondary, 3 primary, 4 trunk/motorway
  int lanes = 0;
  double width = 0;  // metres (0 unknown)
  std::string name;
  std::vector<std::pair<double, double>> pts;
};

double segDist(double px, double pz, std::pair<double, double> a, std::pair<double, double> b) {
  double dx = b.first - a.first, dz = b.second - a.second, l2 = dx * dx + dz * dz;
  double t = l2 > 0 ? std::clamp(((px - a.first) * dx + (pz - a.second) * dz) / l2, 0.0, 1.0) : 0;
  double qx = a.first + t * dx - px, qz = a.second + t * dz - pz;
  return std::sqrt(qx * qx + qz * qz);
}
// the nearest point of a set of polylines
std::pair<double, double> nearestOn(double px, double pz, const std::vector<const RoadIn*>& ws, double& dist) {
  dist = 1e30;
  std::pair<double, double> best{px, pz};
  for (const RoadIn* w : ws)
    for (size_t i = 0; i + 1 < w->pts.size(); ++i) {
      auto a = w->pts[i], b = w->pts[i + 1];
      double dx = b.first - a.first, dz = b.second - a.second, l2 = dx * dx + dz * dz;
      double t = l2 > 0 ? std::clamp(((px - a.first) * dx + (pz - a.second) * dz) / l2, 0.0, 1.0) : 0;
      double qx = a.first + t * dx, qz = a.second + t * dz, d = std::hypot(qx - px, qz - pz);
      if (d < dist) dist = d, best = {qx, qz};
    }
  return best;
}

// the game's road kinds from the OSM tags (owner's request 2026-11-07: wide roads become multi-lane roads, roads with
// trees in the centre avenues). A divided road is two one-way ways of the same name running opposite: the pair
// becomes one road on its centre line, an avenue when a tree row runs between them or the road is tree-lined, else a
// large road (express for motorways). Single ways: motorway/trunk express, 4+ lanes (3+ one-way), 12+ m or
// primary/secondary large, the rest small.
void classifyRoads(std::vector<RoadIn>& roads, const std::vector<std::vector<std::pair<double, double>>>& treeRows,
                   double cellM) {
  std::map<std::string, std::vector<RoadIn*>> byName;
  for (RoadIn& r : roads)
    if (r.oneway && !r.name.empty() && r.kind != 2 && r.pts.size() >= 2) byName[r.name].push_back(&r);
  auto dir = [](const RoadIn& r) {
    double dx = r.pts.back().first - r.pts.front().first, dz = r.pts.back().second - r.pts.front().second;
    double l = std::hypot(dx, dz);
    return l > 0 ? std::pair<double, double>{dx / l, dz / l} : std::pair<double, double>{0, 0};
  };
  const double maxGap = 50.0 / cellM, minGap = 3.0 / cellM;  // carriageways 3-50 m apart
  for (auto& [name, group] : byName)
    for (RoadIn* a : group) {
      if (a->dropped) continue;
      auto da = dir(*a);
      std::vector<const RoadIn*> partners;
      for (RoadIn* b : group) {
        if (b == a || b->dropped) continue;
        auto db = dir(*b);
        if (da.first * db.first + da.second * db.second > -0.5) continue;  // runs the same way
        std::vector<double> ds;
        for (auto& q : b->pts) {
          double d;
          nearestOn(q.first, q.second, {a}, d);
          ds.push_back(d);
        }
        std::nth_element(ds.begin(), ds.begin() + std::ptrdiff_t(ds.size() / 2), ds.end());
        double med = ds[ds.size() / 2];
        if (med >= minGap && med <= maxGap) partners.push_back(b);
      }
      if (partners.empty()) continue;
      // the centre line, and whether a tree row lies inside the strip
      bool trees = a->treeLined;
      std::vector<std::pair<double, double>> mid;
      double gapSum = 0;
      for (auto& q : a->pts) {
        double d;
        auto o = nearestOn(q.first, q.second, partners, d);
        if (d > maxGap) {
          mid.push_back(q);
          continue;
        }
        mid.push_back({(q.first + o.first) * 0.5, (q.second + o.second) * 0.5});
        gapSum += d;
      }
      double gap = gapSum / double(a->pts.size());
      for (const RoadIn* b : partners) trees = trees || b->treeLined;
      if (!trees)
        for (auto& row : treeRows) {
          for (auto& t : row) {
            double best = 1e30;
            for (size_t i = 0; i + 1 < mid.size(); ++i) best = std::min(best, segDist(t.first, t.second, mid[i], mid[i + 1]));
            if (best < gap * 0.5) {
              trees = true;
              break;
            }
          }
          if (trees) break;
        }
      a->pts = std::move(mid);
      a->oneway = false;
      a->kind = a->cls >= 4 ? 4 : trees ? 3 : 1;
      for (const RoadIn* b : partners) const_cast<RoadIn*>(b)->dropped = true;
    }
  for (RoadIn& r : roads) {
    if (r.dropped || r.kind != 0) continue;
    if (r.cls >= 4) r.kind = 4;
    else if (r.treeLined && r.cls >= 1) r.kind = 3;
    else if (r.lanes >= (r.oneway ? 3 : 4) || r.width >= 12.0 || r.cls >= 2) r.kind = 1;
  }
  roads.erase(std::remove_if(roads.begin(), roads.end(), [](const RoadIn& r) { return r.dropped; }), roads.end());
}
struct BuildingIn {
  std::string category;
  uint64_t id;
  int levels;
  std::vector<std::pair<double, double>> pts;
  bool polygon;  // pts is one closed ring (point-in-polygon works)
};
struct ImportData {
  int W = 0, L = 0, flags = 0;
  std::vector<int16_t> heights;  // per corner, engine units (empty: terrain unchanged)
  std::vector<uint8_t> water;    // per cell
  std::vector<std::pair<int, int>> trees;
  std::vector<RoadIn> roads;
  std::vector<BuildingIn> buildings;
  std::vector<uint8_t> satellite;
  int satW = 0, satH = 0;
};

enum ImportFlags { kTerrain = 1, kWater = 2, kTrees = 4, kStreets = 8, kBuildings = 16, kSatellite = 32, kClear = 64 };

struct ImportJob {
  std::mutex mx;
  int state = 0;  // 0 idle, 1 downloading, 2 ready, 3 failed, 4 applying, 5 done
  int step = 0;   // 1 elevation, 2 map data, 3 satellite picture, 4 preparing, 5 terrain, 6 streets, 7 buildings, 8 trees
  float progress = 0;
  std::string error;
  std::shared_ptr<ImportData> data;
  std::atomic<bool> cancel{false};
  int serial = 0;
};
ImportJob g_import;

void setStep(int step, float progress) {
  std::lock_guard<std::mutex> lk(g_import.mx);
  g_import.step = step;
  g_import.progress = progress;
}

struct Frame {  // Mercator ↔ cells
  double cmx, cmy, unitsPerCell;
  int W, L;
  double cellX(double mx) const { return (mx - cmx) / unitsPerCell + W * 0.5; }
  double cellZ(double my) const { return (my - cmy) / unitsPerCell + L * 0.5; }
  double mercX(double cx) const { return cmx + (cx - W * 0.5) * unitsPerCell; }
  double mercY(double cz) const { return cmy + (cz - L * 0.5) * unitsPerCell; }
};

// a mosaic of tiles of one layer and zoom covering [mx0, mx1] × [my0, my1]
struct Mosaic {
  int layer = 0, z = 0, tx0 = 0, ty0 = 0, nx = 0, ny = 0;
  std::vector<std::vector<uint8_t>> tiles;
  bool fetch(double mx0, double my0, double mx1, double my1, int zoom, int lay, int step, float p0, float p1) {
    layer = lay, z = zoom;
    double S = double(1 << z);
    tx0 = int(std::floor(mx0 * S)), ty0 = int(std::floor(my0 * S));
    nx = int(std::floor(mx1 * S)) - tx0 + 1, ny = int(std::floor(my1 * S)) - ty0 + 1;
    tiles.assign(size_t(nx) * size_t(ny), {});
    int done = 0, total = nx * ny, failed = 0;
    std::mutex m;
    std::atomic<int> next{0};
    auto worker = [&] {
      for (;;) {
        int i = next++;
        if (i >= total || g_import.cancel) return;
        int n = 1 << z;
        TileKey k{layer, z, ((tx0 + i % nx) % n + n) % n, std::clamp(ty0 + i / nx, 0, n - 1)};
        std::vector<uint8_t> px;
        bool ok = fetchTile(k, px);
        std::lock_guard<std::mutex> lk(m);
        tiles[size_t(i)] = std::move(px);
        if (!ok) ++failed;
        ++done;
        setStep(step, p0 + (p1 - p0) * float(done) / float(total));
      }
    };
    std::vector<std::thread> ts;
    for (int i = 0; i < (layer == 0 ? 2 : 4); ++i) ts.emplace_back(worker);
    for (auto& t : ts) t.join();
    return !g_import.cancel && failed * 4 < total + 1;
  }
  // bilinear sample of channel bytes at Mercator (mx, my); false outside or missing
  bool sample(double mx, double my, float out[4]) const {
    double S = 256.0 * double(1 << z);
    double px = mx * S - tx0 * 256.0 - 0.5, py = my * S - ty0 * 256.0 - 0.5;
    int x0 = int(std::floor(px)), y0 = int(std::floor(py));
    double fx = px - x0, fy = py - y0;
    float acc[4] = {0, 0, 0, 0};
    for (int k = 0; k < 4; ++k) {
      int x = std::clamp(x0 + (k & 1), 0, nx * 256 - 1), y = std::clamp(y0 + (k >> 1), 0, ny * 256 - 1);
      const std::vector<uint8_t>& t = tiles[size_t(y / 256) * size_t(nx) + size_t(x / 256)];
      if (t.empty()) return false;
      const uint8_t* p = t.data() + (size_t(y % 256) * 256 + size_t(x % 256)) * 4;
      double w = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
      for (int c = 0; c < 4; ++c) acc[c] += float(p[c] * w);
    }
    for (int c = 0; c < 4; ++c) out[c] = acc[c];
    return true;
  }
  // Terrarium height in metres (exact texel values, bilinear over the decoded heights)
  bool height(double mx, double my, double& h) const {
    double S = 256.0 * double(1 << z);
    double px = mx * S - tx0 * 256.0 - 0.5, py = my * S - ty0 * 256.0 - 0.5;
    int x0 = int(std::floor(px)), y0 = int(std::floor(py));
    double fx = px - x0, fy = py - y0, acc = 0;
    for (int k = 0; k < 4; ++k) {
      int x = std::clamp(x0 + (k & 1), 0, nx * 256 - 1), y = std::clamp(y0 + (k >> 1), 0, ny * 256 - 1);
      const std::vector<uint8_t>& t = tiles[size_t(y / 256) * size_t(nx) + size_t(x / 256)];
      if (t.empty()) return false;
      const uint8_t* p = t.data() + (size_t(y % 256) * 256 + size_t(x % 256)) * 4;  // BGRA
      double v = p[2] * 256.0 + p[1] + p[0] / 256.0 - 32768.0;
      acc += v * ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
    }
    h = acc;
    return true;
  }
};

// even-odd fill of a set of edges (rings, unassembled multipolygon members): f(x, z) for every cell whose centre is inside
template <class F>
void fillEdges(const std::vector<std::pair<std::pair<double, double>, std::pair<double, double>>>& edges, int W, int L, F f) {
  if (edges.empty()) return;
  double zmin = 1e30, zmax = -1e30;
  for (auto& e : edges) zmin = std::min({zmin, e.first.second, e.second.second}), zmax = std::max({zmax, e.first.second, e.second.second});
  int z0 = std::max(0, int(std::floor(zmin))), z1 = std::min(L - 1, int(std::ceil(zmax)));
  std::vector<double> xs;
  for (int z = z0; z <= z1; ++z) {
    double cz = z + 0.5;
    xs.clear();
    for (auto& e : edges) {
      double ax = e.first.first, az = e.first.second, bx = e.second.first, bz = e.second.second;
      if ((az <= cz) == (bz <= cz)) continue;
      xs.push_back(ax + (cz - az) * (bx - ax) / (bz - az));
    }
    std::sort(xs.begin(), xs.end());
    for (size_t i = 0; i + 1 < xs.size(); i += 2) {
      int xa = std::max(0, int(std::ceil(xs[i] - 0.5))), xb = std::min(W - 1, int(std::floor(xs[i + 1] - 0.5)));
      for (int x = xa; x <= xb; ++x) f(x, z);
    }
  }
}

using Edges = std::vector<std::pair<std::pair<double, double>, std::pair<double, double>>>;
void addRing(Edges& e, const std::vector<std::pair<double, double>>& pts, bool close) {
  for (size_t i = 0; i + 1 < pts.size(); ++i) e.push_back({pts[i], pts[i + 1]});
  if (close && pts.size() > 2 && pts.front() != pts.back()) e.push_back({pts.back(), pts.front()});
}

// the cells a segment passes (4-connected, so a flood fill cannot leak through it)
template <class F>
void lineCells(double ax, double az, double bx, double bz, F f) {
  double dx = bx - ax, dz = bz - az;
  int steps = int(std::ceil(std::max(std::abs(dx), std::abs(dz)) * 2.0)) + 1;
  int px = int(std::floor(ax)), pz = int(std::floor(az));
  f(px, pz);
  for (int i = 1; i <= steps; ++i) {
    double t = double(i) / steps;
    int x = int(std::floor(ax + dx * t)), z = int(std::floor(az + dz * t));
    if (x != px && z != pz) f(x, pz);  // the corner step through a side cell
    if (x != px || z != pz) f(x, z);
    px = x, pz = z;
  }
}

uint32_t hash32(uint64_t v) {
  v ^= v >> 33;
  v *= 0xff51afd7ed558ccdull;
  v ^= v >> 33;
  v *= 0xc4ceb9fe1a85ec53ull;
  v ^= v >> 33;
  return uint32_t(v);
}

std::string buildingCategory(const Json& tags, int& levels) {
  std::string b = tags.str("building");
  levels = int(tags.num("building:levels", 0));
  static const std::map<std::string, std::string> kMap = {
      {"house", "residential"},       {"detached", "residential"},  {"semidetached_house", "residential"},
      {"terrace", "residential"},     {"residential", "residential"}, {"bungalow", "residential"},
      {"apartments", "blockhouses"},  {"dormitory", "blockhouses"}, {"commercial", "commercial"},
      {"retail", "commercial"},       {"supermarket", "commercial"}, {"hotel", "commercial"},
      {"kiosk", "commercial"},        {"office", "workplace"},      {"industrial", "industry"},
      {"warehouse", "industry"},      {"factory", "industry"},      {"manufacture", "industry"},
      {"hangar", "industry"},         {"farm", "farm"},             {"barn", "farm"},
      {"farm_auxiliary", "farm"},     {"stable", "farm"},           {"greenhouse", "farm"},
      {"church", "government"},       {"cathedral", "government"},  {"chapel", "government"},
      {"public", "government"},       {"civic", "government"},      {"government", "government"},
      {"school", "government"},       {"university", "government"}, {"college", "government"},
      {"hospital", "government"},     {"townhall", "government"},   {"kindergarten", "government"},
      {"train_station", "railway"},   {"transportation", "railway"}};
  static const std::set<std::string> kSkip = {"garage", "garages", "shed", "roof", "carport", "hut", "ruins",
                                              "construction", "container", "toilets", "bunker", "no"};
  if (kSkip.count(b)) return {};
  auto it = kMap.find(b);
  if (it != kMap.end()) return it->second;
  return "?";  // yes and others: from the land use around, else residential
}

void runImport(int serial, Frame fr, int flags, std::string* errorOut, std::shared_ptr<ImportData>& result) {
  auto data = std::make_shared<ImportData>();
  data->W = fr.W, data->L = fr.L, data->flags = flags;
  const int W = fr.W, L = fr.L;
  double mx0 = fr.mercX(0), mx1 = fr.mercX(W), my0 = fr.mercY(0), my1 = fr.mercY(L);
  double south = myToLat(my1), north = myToLat(my0), west = mxToLon(mx0), east = mxToLon(mx1);
  auto fail = [&](const std::string& e) {
    *errorOut = e;
    return;
  };
  // 1. elevation
  std::vector<double> elev;
  bool haveElev = false;
  if (flags & (kTerrain | kWater)) {
    int ze = 14;
    for (; ze > 4; --ze) {
      double S = double(1 << ze);
      long n = long(std::floor(mx1 * S) - std::floor(mx0 * S) + 1) * long(std::floor(my1 * S) - std::floor(my0 * S) + 1);
      if (n <= 36) break;
    }
    Mosaic m;
    setStep(1, 0);
    if (m.fetch(mx0, my0, mx1, my1, ze, 2, 1, 0, 1)) {
      elev.assign(size_t(W) * size_t(L), 0.0);
      haveElev = true;
      for (int z = 0; z < L && haveElev; ++z)
        for (int x = 0; x < W; ++x) {
          double h = 0;
          if (!m.height(fr.mercX(x), fr.mercY(z), h)) h = 0;
          elev[size_t(z) * size_t(W) + size_t(x)] = h;
        }
    } else if (g_import.cancel) {
      return;
    } else if (flags & kTerrain) {
      return fail("elevation");
    }
  }
  // 2. map data
  data->water.assign(size_t(W) * size_t(L), 0);
  Json osm;
  bool haveOsm = false;
  if (flags & (kWater | kTrees | kStreets | kBuildings)) {
    setStep(2, 0);
    char bbox[160];
    std::snprintf(bbox, sizeof bbox, "(%.7f,%.7f,%.7f,%.7f)", south, west, north, east);
    std::string b = bbox, q = "[out:json][timeout:120];(";
    if (flags & kStreets)
      q += "way[\"highway\"~\"^(motorway|trunk|primary|secondary|tertiary|unclassified|residential|living_street|"
           "motorway_link|trunk_link|primary_link|secondary_link|tertiary_link|pedestrian)$\"]" + b + ";way[\"natural\"=\"tree_row\"]" + b + ";";
    if (flags & kBuildings)
      q += "way[\"building\"]" + b + ";relation[\"building\"]" + b + ";way[\"landuse\"~\"^(residential|commercial|retail|"
           "industrial)$\"]" + b + ";";
    if (flags & kWater)
      q += "way[\"natural\"~\"^(water|coastline)$\"]" + b + ";relation[\"natural\"=\"water\"]" + b +
           ";way[\"landuse\"~\"^(reservoir|basin)$\"]" + b + ";way[\"waterway\"~\"^(river|canal|riverbank|dock)$\"]" + b +
           ";relation[\"waterway\"=\"riverbank\"]" + b + ";";
    if (flags & kTrees)
      q += "way[\"natural\"~\"^(wood|scrub)$\"]" + b + ";relation[\"natural\"=\"wood\"]" + b +
           ";way[\"landuse\"~\"^(forest|orchard)$\"]" + b + ";relation[\"landuse\"=\"forest\"]" + b +
           ";way[\"leisure\"~\"^(park|garden)$\"]" + b + ";relation[\"leisure\"=\"park\"]" + b + ";node[\"natural\"=\"tree\"]" +
           b + ";";
    q += ");out geom;";
    std::string body = "data=" + urlEncode(q);
    std::vector<uint8_t> answer;
    std::string err;
    int status = 0;
    // answers are kept for a week by the query (the same area imported again, or another map size of it, needs no
    // server); the public servers are often busy, so two rounds over three of them
    uint64_t h = 1469598103934665603ull;
    for (unsigned char ch : q) h = (h ^ ch) * 1099511628211ull;
    char name[32];
    std::snprintf(name, sizeof name, "%016llx.json", (unsigned long long)h);
    std::filesystem::path cached = cacheRoot() / "overpass" / name;
    std::error_code ec;
    bool fresh = std::filesystem::exists(cached, ec) &&
                 std::filesystem::file_time_type::clock::now() - std::filesystem::last_write_time(cached, ec) < std::chrono::hours(24 * 7);
    if (fresh && readFile(cached, answer)) status = 200;
    for (int round = 0; round < 2 && status != 200 && !g_import.cancel; ++round) {
      if (round) std::this_thread::sleep_for(std::chrono::seconds(5));
      for (const char* server : {"https://overpass-api.de/api/interpreter", "https://maps.mail.ru/osm/tools/overpass/api/interpreter",
                                 "https://overpass.private.coffee/api/interpreter"}) {
        status = platformHttpRequest(server, &body, answer, &err);
        if (status == 200 || g_import.cancel) break;
        LOG_WARN("osm: overpass %s: status %d %s", server, status, err.c_str());
      }
      if (status == 200) writeFile(cached, answer);
    }
    if (g_import.cancel) return;
    if (status != 200) return fail(status == 0 ? "connection" : "server " + std::to_string(status));
    setStep(2, 0.6f);
    if (!parseJson(answer, osm)) return fail("data");
    haveOsm = true;
    LOG_INFO("osm: %zu bytes of map data", answer.size());
  }
  setStep(4, 0);
  auto P = [&](const Json& g) {  // a geometry array → cell coordinates
    std::vector<std::pair<double, double>> pts;
    if (g.t != Json::Arr) return pts;
    pts.reserve(g.a.size());
    for (const Json& n : g.a)
      if (n.t == Json::Obj) pts.push_back({fr.cellX(lonToMx(n.num("lon"))), fr.cellZ(latToMy(n.num("lat")))});
    return pts;
  };
  Edges waterEdges, coast, landuseRes, landuseCom, landuseInd;
  std::vector<std::vector<std::pair<double, double>>> treeRows;
  std::vector<std::pair<Edges, double>> treeAreas;
  std::vector<std::pair<std::vector<std::pair<double, double>>, double>> rivers;  // line, width in cells
  const double cellM = fr.unitsPerCell * metresPerUnit(fr.cmy);
  if (haveOsm)
    if (const Json* els = osm.get("elements"); els && els->t == Json::Arr)
      for (const Json& el : els->a) {
        const Json* tagsP = el.get("tags");
        static const Json kEmpty;
        const Json& tags = tagsP ? *tagsP : kEmpty;
        std::string type = el.str("type");
        if (type == "node") {
          if ((flags & kTrees) && tags.str("natural") == "tree") {
            int x = int(std::floor(fr.cellX(lonToMx(el.num("lon"))))), z = int(std::floor(fr.cellZ(latToMy(el.num("lat")))));
            if (x >= 1 && z >= 1 && x < W - 1 && z < L - 1) data->trees.push_back({x, z});
          }
          continue;
        }
        // the way's points, or a relation's members (outer and inner rings as edges)
        std::vector<std::pair<double, double>> pts;
        Edges ringEdges;
        bool closed = false;
        if (type == "way") {
          if (const Json* g = el.get("geometry")) pts = P(*g);
          closed = pts.size() > 3 && pts.front() == pts.back();
          addRing(ringEdges, pts, true);
        } else if (type == "relation") {
          if (const Json* mem = el.get("members"); mem && mem->t == Json::Arr)
            for (const Json& m : mem->a)
              if (const Json* g = m.get("geometry")) {
                auto mp = P(*g);
                addRing(ringEdges, mp, false);
                if (m.str("role") != "inner") pts.insert(pts.end(), mp.begin(), mp.end());
              }
          closed = true;
        }
        if (pts.size() < 2) continue;
        std::string hw = tags.str("highway"), nat = tags.str("natural"), lu = tags.str("landuse"), ww = tags.str("waterway"),
                    le = tags.str("leisure");
        if (!hw.empty() && type == "way") {
          if (tags.str("tunnel") == "yes" || tags.str("tunnel") == "building_passage" || tags.num("layer", 0) < 0) continue;
          if (tags.str("area") == "yes") continue;
          RoadIn r;
          r.kind = hw == "pedestrian" ? 2 : 0;
          r.cls = (hw == "motorway" || hw == "trunk") ? 4 : hw == "primary" ? 3 : hw == "secondary" ? 2 : hw == "tertiary" ? 1 : 0;
          r.bridge = tags.str("bridge") == "yes" || tags.str("bridge") == "viaduct";
          std::string ow = tags.str("oneway");
          r.oneway = ow == "yes" || ow == "1" || ow == "-1" || hw == "motorway";
          r.lanes = int(tags.num("lanes", 0));
          r.width = std::atof(tags.str("width").c_str());
          std::string tl = tags.str("tree_lined"), med = tags.str("median");
          r.treeLined = tl == "yes" || tl == "both" || tl == "median" || med.find("tree") != std::string::npos;
          r.name = tags.str("name");
          if (r.name.empty()) r.name = tags.str("ref");
          r.pts = std::move(pts);
          data->roads.push_back(std::move(r));
          continue;
        }
        if (!tags.str("building").empty() && (flags & kBuildings)) {
          BuildingIn bi;
          bi.category = buildingCategory(tags, bi.levels);
          if (bi.category.empty()) continue;
          bi.id = uint64_t(el.num("id"));
          bi.polygon = type == "way";
          if (bi.polygon && pts.size() > 1 && pts.front() == pts.back()) pts.pop_back();
          bi.pts = std::move(pts);
          data->buildings.push_back(std::move(bi));
          continue;
        }
        if (nat == "tree_row") {
          if (flags & kTrees)  // the row's trees: about one every 8 m
            for (size_t i = 0; i + 1 < pts.size(); ++i) {
              double len = std::hypot(pts[i + 1].first - pts[i].first, pts[i + 1].second - pts[i].second);
              int n = std::max(1, int(len * cellM / 8.0));
              for (int k = 0; k < n; ++k) {
                double t = (k + 0.5) / n;
                int x = int(std::floor(pts[i].first + (pts[i + 1].first - pts[i].first) * t));
                int z = int(std::floor(pts[i].second + (pts[i + 1].second - pts[i].second) * t));
                if (x >= 1 && z >= 1 && x < W - 1 && z < L - 1) data->trees.push_back({x, z});
              }
            }
          treeRows.push_back(std::move(pts));
          continue;
        }
        if (lu == "residential") addRing(landuseRes, pts, true);
        else if (lu == "commercial" || lu == "retail") addRing(landuseCom, pts, true);
        else if (lu == "industrial") addRing(landuseInd, pts, true);
        if (nat == "coastline") {
          for (size_t i = 0; i + 1 < pts.size(); ++i) coast.push_back({pts[i], pts[i + 1]});
        } else if (nat == "water" || lu == "reservoir" || lu == "basin" || ww == "riverbank" || ww == "dock") {
          if (closed) waterEdges.insert(waterEdges.end(), ringEdges.begin(), ringEdges.end());
        } else if (ww == "river" || ww == "canal") {
          double wm = tags.num("width", ww == "river" ? 15.0 : 10.0);
          rivers.push_back({std::move(pts), wm / cellM});
        } else if (nat == "wood" || lu == "forest") {
          if (closed) treeAreas.push_back({ringEdges, 0.35});
        } else if (nat == "scrub" || lu == "orchard") {
          if (closed) treeAreas.push_back({ringEdges, 0.15});
        } else if (le == "park" || le == "garden") {
          if (closed) treeAreas.push_back({ringEdges, 0.06});
        }
      }
  osm = Json();
  classifyRoads(data->roads, treeRows, cellM);
  // water: lakes and river areas, river lines, the sea behind the coastline
  std::vector<uint8_t>& water = data->water;
  if (flags & kWater) {
    fillEdges(waterEdges, W, L, [&](int x, int z) { water[size_t(z) * size_t(W) + size_t(x)] = 1; });
    for (auto& [line, wc] : rivers) {
      double r = std::max(0.5, wc * 0.5);
      for (size_t i = 0; i + 1 < line.size(); ++i) {
        auto [ax, az] = line[i];
        auto [bx, bz] = line[i + 1];
        int x0 = std::max(0, int(std::floor(std::min(ax, bx) - r))), x1 = std::min(W - 1, int(std::ceil(std::max(ax, bx) + r)));
        int z0 = std::max(0, int(std::floor(std::min(az, bz) - r))), z1 = std::min(L - 1, int(std::ceil(std::max(az, bz) + r)));
        double dx = bx - ax, dz = bz - az, len2 = dx * dx + dz * dz;
        for (int z = z0; z <= z1; ++z)
          for (int x = x0; x <= x1; ++x) {
            double px = x + 0.5 - ax, pz = z + 0.5 - az;
            double t = len2 > 0 ? std::clamp((px * dx + pz * dz) / len2, 0.0, 1.0) : 0;
            double qx = px - t * dx, qz = pz - t * dz;
            if (qx * qx + qz * qz <= r * r) water[size_t(z) * size_t(W) + size_t(x)] = 1;
          }
      }
    }
    if (!coast.empty()) {
      // the coastline as a wall; the sea on its right side is flooded from seeds beside it
      std::vector<uint8_t> wall(size_t(W) * size_t(L), 0), sea(size_t(W) * size_t(L), 0);
      auto in = [&](int x, int z) { return x >= 0 && z >= 0 && x < W && z < L; };
      for (auto& e : coast)
        lineCells(e.first.first, e.first.second, e.second.first, e.second.second, [&](int x, int z) {
          if (in(x, z)) wall[size_t(z) * size_t(W) + size_t(x)] = 1;
        });
      std::vector<std::pair<int, int>> stack;
      for (auto& e : coast) {
        double dx = e.second.first - e.first.first, dz = e.second.second - e.first.second;
        double len = std::sqrt(dx * dx + dz * dz);
        if (len < 1e-9) continue;
        double nx = -dz / len, nz = dx / len;  // the right side in cell coordinates (x east, z south)
        for (double t : {0.25, 0.5, 0.75}) {
          int x = int(std::floor(e.first.first + dx * t + nx * 1.2)), z = int(std::floor(e.first.second + dz * t + nz * 1.2));
          if (in(x, z) && !wall[size_t(z) * size_t(W) + size_t(x)]) stack.push_back({x, z});
        }
      }
      while (!stack.empty()) {
        auto [x, z] = stack.back();
        stack.pop_back();
        size_t i = size_t(z) * size_t(W) + size_t(x);
        if (sea[i] || wall[i]) continue;
        sea[i] = 1;
        for (auto [nx, nz] : {std::pair{x - 1, z}, {x + 1, z}, {x, z - 1}, {x, z + 1}})
          if (in(nx, nz)) stack.push_back({nx, nz});
      }
      size_t count = 0;
      for (int z = 0; z < L; ++z)
        for (int x = 0; x < W; ++x) {
          size_t i = size_t(z) * size_t(W) + size_t(x);
          if (wall[i]) {  // the wall's cells go with their neighbours
            int n = 0;
            for (auto [nx, nz] : {std::pair{x - 1, z}, {x + 1, z}, {x, z - 1}, {x, z + 1}})
              if (in(nx, nz) && sea[size_t(nz) * size_t(W) + size_t(nx)]) ++n;
            if (n >= 2) water[i] = 1;
          }
          if (sea[i]) water[i] = 1, ++count;
        }
      LOG_INFO("osm: sea %zu cells", count);
    } else if (haveElev) {  // open water without a coastline in the area: deep cells
      for (size_t i = 0; i < water.size(); ++i)
        if (elev[i] < -10.0) water[i] = 1;
    }
  }
  // terrain heights: land from its lowest point at the ground level 0x200, water below the water line
  if ((flags & kTerrain) && haveElev) {
    std::vector<double> land;
    for (size_t i = 0; i < elev.size(); ++i)
      if (!water[i]) land.push_back(elev[i]);
    double base = 0;
    if (!land.empty()) {
      std::nth_element(land.begin(), land.begin() + std::ptrdiff_t(land.size() / 50), land.end());
      base = land[land.size() / 50];
    }
    double perUnit = 256.0 / cellM;  // engine height units per metre (vertical scale = horizontal scale)
    data->heights.resize(elev.size());
    for (size_t i = 0; i < elev.size(); ++i)
      data->heights[i] = int16_t(std::clamp(0x200 + (elev[i] - base) * perUnit, double(0x180), double(0x7000)));
  }
  // trees: the areas' cells at their density
  if (flags & kTrees) {
    std::vector<uint8_t> taken(size_t(W) * size_t(L), 0);
    for (auto& t : data->trees) taken[size_t(t.second) * size_t(W) + size_t(t.first)] = 1;
    uint64_t salt = 0;
    for (auto& [edges, density] : treeAreas) {
      ++salt;
      double d = density;
      fillEdges(edges, W, L, [&](int x, int z) {
        size_t i = size_t(z) * size_t(W) + size_t(x);
        if (taken[i] || water[i] || x < 1 || z < 1 || x >= W - 1 || z >= L - 1) return;
        if (double(hash32((uint64_t(x) << 40) ^ (uint64_t(z) << 16) ^ salt) % 10000) >= d * 10000.0) return;
        taken[i] = 1;
        data->trees.push_back({x, z});
      });
    }
    if (data->trees.size() > 60000) {  // the game's maps hold about 90k objects in all
      for (size_t i = data->trees.size() - 1; i > 0; --i) std::swap(data->trees[i], data->trees[hash32(i) % (i + 1)]);
      data->trees.resize(60000);
    }
  }
  // buildings without a clear kind: the land use around them
  for (BuildingIn& b : data->buildings) {
    if (b.category != "?") continue;
    double cx = 0, cz = 0;
    for (auto& p : b.pts) cx += p.first, cz += p.second;
    cx /= double(b.pts.size()), cz /= double(b.pts.size());
    auto inside = [&](const Edges& e) {
      int n = 0;
      for (auto& s : e) {
        double ax = s.first.first, az = s.first.second, bx = s.second.first, bz = s.second.second;
        if ((az <= cz) != (bz <= cz) && cx < ax + (cz - az) * (bx - ax) / (bz - az)) ++n;
      }
      return (n & 1) != 0;
    };
    if (inside(landuseCom)) b.category = "commercial";
    else if (inside(landuseInd)) b.category = "industry";
    else b.category = b.levels >= 4 ? "blockhouses" : "residential";
  }
  // 3. the satellite picture
  if (flags & kSatellite) {
    int pw = std::min(4096, W * 4), ph = std::min(4096, L * 4);
    double areaPx = (mx1 - mx0) * 256.0;
    int zs = std::clamp(int(std::ceil(std::log2(double(pw) / areaPx))), 1, 19);
    for (;; --zs) {
      double S = double(1 << zs);
      long n = long(std::floor(mx1 * S) - std::floor(mx0 * S) + 1) * long(std::floor(my1 * S) - std::floor(my0 * S) + 1);
      if (n <= 900 || zs <= 1) break;
    }
    Mosaic m;
    setStep(3, 0);
    if (m.fetch(mx0, my0, mx1, my1, zs, 1, 3, 0, 1)) {
      data->satellite.assign(size_t(pw) * size_t(ph) * 4, 0xFF);
      for (int y = 0; y < ph; ++y)
        for (int x = 0; x < pw; ++x) {
          float c[4];
          if (!m.sample(fr.mercX((x + 0.5) * W / double(pw)), fr.mercY((y + 0.5) * L / double(ph)), c)) continue;
          uint8_t* p = data->satellite.data() + (size_t(y) * size_t(pw) + size_t(x)) * 4;
          p[0] = uint8_t(c[0]), p[1] = uint8_t(c[1]), p[2] = uint8_t(c[2]), p[3] = 0xFF;
        }
      data->satW = pw, data->satH = ph;
    } else if (g_import.cancel) {
      return;
    }
  }
  LOG_INFO("osm: import data: %zu roads, %zu buildings, %zu trees, terrain %d, satellite %dx%d", data->roads.size(),
           data->buildings.size(), data->trees.size(), int(!data->heights.empty()), data->satW, data->satH);
  (void)serial;
  result = data;
}

// ---------------------------------------------------------------------------------------------------------------
// applying the import to the map (main thread, a slice per call)

struct Drag {
  int editMode, toolMode, x0, z0, x1, z1;
};

struct Apply {
  std::shared_ptr<ImportData> d;
  int stage = 0;
  size_t next = 0;
  std::vector<Drag> drags;  // alleys
  std::map<std::pair<int, int>, uint32_t> network;
  std::vector<uint8_t> used;  // cells taken by roads, buildings, trees, water
  std::vector<int> made;
  Object* timeline = nullptr;
  int roadsOk = 0, roadsFailed = 0, buildingsPlaced = 0, treesPlaced = 0;
};
std::unique_ptr<Apply> g_apply;

// the road network on the block grid (owner's requests 2026-11-07: streets connected, the map making sense). Every way
// is simplified (keeping the points other ways share, the junctions), its points snapped to blocks (the large roads
// and express roads to their lattice (x + z) & 4 == 0, avenues to every 8 cells), ways sharing an OSM node share the
// block, and the blocks between two points are walked as the road tool walks a drag: straight, then diagonal (wide
// roads two blocks per straight step, avenues never diagonal). Each step sets the connection bit of its kind on both
// blocks. Ways leaving the map end on the edge block (the piece there leads off the map); blocks in water are left out
// (bridges come later). Parts of the network that touch neither the largest part nor the map edge are dropped
// (toolBuildRoadNetwork, also after it removed colliding pieces).
// Pedestrian streets stay drags of the alley tool.
using Blocks = std::map<std::pair<int, int>, uint32_t>;

int sgn(int v) { return v < 0 ? -1 : v > 0 ? 1 : 0; }
// the road tool's direction bits (1 north = −z, 2 north-east, 4 east, … 0x80 north-west)
uint32_t dirBit(int vz, int vx) {
  if (vz == 0 || vx == 0) {
    if (vz < 0) return 1;
    if (vx > 0) return 4;
    if (vz > 0) return 0x10;
    return vx < 0 ? 0x40 : 0;
  }
  if (vx > 0) return vz < 0 ? 2 : 8;
  return vz > 0 ? 0x20 : 0x80;
}
void bitStep(uint32_t b, int& dz, int& dx) {
  switch (b) {
    case 1: dz = -1, dx = 0; break;
    case 2: dz = -1, dx = 1; break;
    case 4: dz = 0, dx = 1; break;
    case 8: dz = 1, dx = 1; break;
    case 0x10: dz = 1, dx = 0; break;
    case 0x20: dz = 1, dx = -1; break;
    case 0x40: dz = 0, dx = -1; break;
    case 0x80: dz = -1, dx = -1; break;
    default: break;
  }
}

int kindIndex(int kind) { return kind == 1 ? 1 : kind == 3 ? 2 : kind == 4 ? 3 : 0; }  // the mask byte

void buildNetwork(const std::vector<RoadIn>& roads, int W, int L, const std::vector<uint8_t>& water, Blocks& blocks,
                  std::vector<Drag>& alleys) {
  auto key = [](std::pair<double, double> q) { return std::pair<long long, long long>{std::llround(q.first * 64), std::llround(q.second * 64)}; };
  std::map<std::pair<long long, long long>, int> shared;
  std::map<std::pair<long long, long long>, std::pair<int, int>> nodeBlock;
  for (const RoadIn& r : roads)
    if (r.kind != 2)
      for (auto& q : r.pts) ++shared[key(q)];
  std::vector<const RoadIn*> order;
  for (const RoadIn& r : roads) order.push_back(&r);
  std::stable_sort(order.begin(), order.end(), [](const RoadIn* x, const RoadIn* y) { return kindIndex(x->kind) > kindIndex(y->kind); });
  const int maxX = W - 4, maxZ = L - 4;
  auto inside = [&](int x, int z) { return x >= 0 && z >= 0 && x <= maxX && z <= maxZ; };
  // nodes keep a block away from the edge; only a way that leaves the map reaches it, at a right angle (the game's
  // edge pieces lead straight off the map)
  auto innerOk = [&](int x, int z) { return x >= 4 && z >= 4 && x <= maxX - 4 && z <= maxZ - 4; };
  auto wet = [&](int bx, int bz) {
    int cx = std::clamp(bx + 2, 0, W - 1), cz = std::clamp(bz + 2, 0, L - 1);
    return water[size_t(cz) * size_t(W) + size_t(cx)] != 0;
  };
  for (const RoadIn* rp : order) {
    const RoadIn& r = *rp;
    if (r.pts.size() < 2) continue;
    if (r.kind == 2) {  // alleys: one drag per leg on the 1-cell grid
      for (size_t i = 0; i + 1 < r.pts.size(); ++i) {
        int x0 = int(std::floor(r.pts[i].first)), z0 = int(std::floor(r.pts[i].second));
        int x1 = int(std::floor(r.pts[i + 1].first)), z1 = int(std::floor(r.pts[i + 1].second));
        if (x0 < 1 || z0 < 1 || x1 < 1 || z1 < 1 || x0 >= W - 2 || x1 >= W - 2 || z0 >= L - 2 || z1 >= L - 2) continue;
        if (x0 != x1 || z0 != z1) alleys.push_back({0x12, 0, x0, z0, x1, z1});
      }
      continue;
    }
    const int ki = kindIndex(r.kind);
    const bool lattice = ki == 1 || ki == 3, grid8 = ki == 2;
    // simplify (one block), keeping shared points; split long legs
    std::vector<std::pair<double, double>> pts = r.pts;
    std::vector<char> keepPt(pts.size(), 0);
    keepPt.front() = keepPt.back() = 1;
    for (size_t i = 0; i < pts.size(); ++i)
      if (shared[key(pts[i])] > 1) keepPt[i] = 1;
    std::vector<std::pair<size_t, size_t>> st;
    size_t prev = 0;
    for (size_t i = 1; i < pts.size(); ++i)
      if (keepPt[i]) st.push_back({prev, i}), prev = i;
    while (!st.empty()) {
      auto [ia, ib] = st.back();
      st.pop_back();
      double ax = pts[ia].first, az = pts[ia].second, bx = pts[ib].first, bz = pts[ib].second;
      double dx = bx - ax, dz = bz - az, len = std::sqrt(dx * dx + dz * dz);
      size_t best = 0;
      double bd = 0;
      for (size_t i = ia + 1; i < ib; ++i) {
        double d = len > 1e-9 ? std::abs((pts[i].first - ax) * dz - (pts[i].second - az) * dx) / len
                              : std::hypot(pts[i].first - ax, pts[i].second - az);
        if (d > bd) bd = d, best = i;
      }
      if (bd > 3.5) {
        keepPt[best] = 1;
        st.push_back({ia, best});
        st.push_back({best, ib});
      }
    }
    struct Node {
      double x, z;
      bool original;
      std::pair<long long, long long> k;
    };
    std::vector<Node> nodes;
    const double maxLeg = 24.0;
    for (size_t i = 0; i < pts.size(); ++i) {
      if (!keepPt[i]) continue;
      if (!nodes.empty()) {
        double dx = pts[i].first - nodes.back().x, dz = pts[i].second - nodes.back().z;
        int parts = int(std::ceil(std::sqrt(dx * dx + dz * dz) / maxLeg));
        double bx = nodes.back().x, bz = nodes.back().z;
        for (int k = 1; k < parts; ++k) nodes.push_back({bx + dx * k / parts, bz + dz * k / parts, false, {}});
      }
      nodes.push_back({pts[i].first, pts[i].second, true, key(pts[i])});
    }
    auto snap = [&](double x, double z) {
      if (grid8) return std::pair<int, int>{int(std::lround((x - 4) / 8)) * 8, int(std::lround((z - 4) / 8)) * 8};
      int bx = int(std::lround((x - 2) / 4)) * 4, bz = int(std::lround((z - 2) / 4)) * 4;
      if (lattice && ((bx + bz) & 4)) {
        double fx = (x - 2) / 4 - bx / 4, fz = (z - 2) / 4 - bz / 4;
        if (std::abs(fx) > std::abs(fz)) bx += fx > 0 ? 4 : -4;
        else bz += fz > 0 ? 4 : -4;
      }
      return std::pair<int, int>{bx, bz};
    };
    // the block of an edge crossing: on the border, the other coordinate snapped to the kind's grid
    auto borderBlock = [&](double ax, double az, double bx, double bz) {
      double t = 1.0;
      auto clip = [&](double a0, double a1, double lo, double hi) {
        double d = a1 - a0;
        if (a1 < lo && d != 0) t = std::min(t, (lo - a0) / d);
        if (a1 > hi && d != 0) t = std::min(t, (hi - a0) / d);
      };
      clip(ax, bx, 2.0, maxX + 2.0);
      clip(az, bz, 2.0, maxZ + 2.0);
      double px = ax + (bx - ax) * t, pz = az + (bz - az) * t;
      auto s2 = snap(px, pz);
      int x = std::clamp(s2.first, 0, maxX), z = std::clamp(s2.second, 0, maxZ);
      if (bx < 2.0 || bx > maxX + 2.0) x = bx < 2.0 ? 0 : maxX;
      else z = bz < 2.0 ? 0 : maxZ;
      if (grid8) x -= x % 8, z -= z % 8;
      else if (lattice && ((x + z) & 4)) (x == 0 || x == maxX) ? (z = z + 4 <= maxZ ? z + 4 : z - 4) : (x = x + 4 <= maxX ? x + 4 : x - 4);
      return std::pair<int, int>{x, z};
    };
    std::vector<std::pair<int, int>> bl;  // the snapped nodes, {-1,-1} where the way is off the map
    for (size_t i = 0; i < nodes.size(); ++i) {
      std::pair<int, int> n;
      auto it = nodes[i].original ? nodeBlock.find(nodes[i].k) : nodeBlock.end();
      if (it != nodeBlock.end()) n = it->second;
      else n = snap(nodes[i].x, nodes[i].z);
      const int perp = (lattice || grid8) ? 8 : 4;
      auto innerOf = [&](std::pair<int, int> e) {
        int x = e.first, z = e.second;
        if (x == 0) x += perp;
        else if (x == maxX) x -= perp;
        else if (z == 0) z += perp;
        else z -= perp;
        return std::pair<int, int>{x, z};
      };
      bool in = innerOk(n.first, n.second);
      if (!in) {
        // leaving: the block inside the edge, then the edge block
        if (i > 0 && !bl.empty() && bl.back().first >= 0) {
          auto e = borderBlock(nodes[i - 1].x, nodes[i - 1].z, nodes[i].x, nodes[i].z);
          bl.push_back(innerOf(e));
          bl.push_back(e);
        }
        bl.push_back({-1, -1});
        continue;
      }
      if (i > 0 && !bl.empty() && bl.back().first < 0) {  // entering: the edge block, then the block inside it
        auto e = borderBlock(nodes[i].x, nodes[i].z, nodes[i - 1].x, nodes[i - 1].z);
        bl.push_back(e);
        bl.push_back(innerOf(e));
      }
      if (nodes[i].original && it == nodeBlock.end()) nodeBlock[nodes[i].k] = n;
      if (bl.empty() || bl.back() != n) bl.push_back(n);
    }
    // walk the legs
    const int S = 8 * ki;
    for (size_t i = 0; i + 1 < bl.size(); ++i) {
      auto [cx, cz] = bl[i];
      auto [ex, ez] = bl[i + 1];
      if (cx < 0 || ex < 0 || (cx == ex && cz == ez)) continue;
      if (wet(cx, cz)) continue;
      for (int guard = 0; guard < 4096; ++guard) {
        int ax = ex - cx, az = ez - cz;
        if (ax == 0 && az == 0) break;
        int dx, dz, step = 4;
        if (!grid8 && ax != 0 && az != 0 && std::abs(ax) == std::abs(az)) dx = sgn(ax), dz = sgn(az);
        else if (std::abs(ax) >= std::abs(az)) dx = sgn(ax), dz = 0, step = (lattice || grid8) ? 8 : 4;
        else dz = sgn(az), dx = 0, step = (lattice || grid8) ? 8 : 4;
        if (dx && std::abs(dx * step) > std::abs(ax)) step = std::abs(ax);
        if (dz && std::abs(dz * step) > std::abs(az)) step = std::abs(az);
        int nx = cx + dx * step, nz = cz + dz * step;
        if (!inside(nx, nz) || wet(nx, nz)) break;
        uint32_t fwd = dirBit(dz, dx), back = dirBit(-dz, -dx);
        blocks[{cx, cz}] |= fwd << S;
        blocks[{nx, nz}] |= back << S;
        cx = nx, cz = nz;
      }
    }
  }
  // (the parts not joined to the largest one or the map edge are dropped by toolBuildRoadNetwork)
}

// the ground under the street network levelled, as roads are built (the game's road pieces lie flat on a block and
// meet their neighbours at the same height; bumps of the real terrain under them make them collide): each connected
// part of the network gets one height, the median of the ground under its blocks in half-cell steps; the ground
// around it eases into that height over 4 cells. Hilly places get their streets on a levelled plateau for now (ramps
// along the streets would need the road tool's slope pieces placed in order).
void levelUnderRoads(GridData& g, const std::map<std::pair<int, int>, uint32_t>& net, const std::vector<uint8_t>& water,
                     std::vector<int16_t>& heights) {
  const int W = g.width, L = g.length;
  std::map<std::pair<int, int>, int> comp;
  std::vector<std::vector<std::pair<int, int>>> parts;
  for (auto& [k, m] : net) {
    if (!m || comp.count(k)) continue;
    int id = int(parts.size());
    parts.emplace_back();
    std::vector<std::pair<int, int>> stack{k};
    comp[k] = id;
    while (!stack.empty()) {
      auto at = stack.back();
      stack.pop_back();
      parts.back().push_back(at);
      uint32_t mm = net.at(at);
      for (int b = 0; b < 32; ++b) {
        if (!((mm >> b) & 1)) continue;
        int dz = 0, dx = 0;
        bitStep(1u << (b % 8), dz, dx);
        int st = (b / 8 == 0) ? 4 : (dx && dz) ? 4 : 8;
        std::pair<int, int> nb{at.first + dx * st, at.second + dz * st};
        if (net.count(nb) && !comp.count(nb)) comp[nb] = id, stack.push_back(nb);
      }
    }
  }
  auto H = [&](int x, int z) -> int16_t& { return g.height[size_t(z) * size_t(W) + size_t(x)]; };
  auto wetCorner = [&](int x, int z) {
    for (int dz = -1; dz <= 0; ++dz)
      for (int dx = -1; dx <= 0; ++dx) {
        int cx = x + dx, cz = z + dz;
        if (cx >= 0 && cz >= 0 && cx < W && cz < L && water[size_t(cz) * size_t(W) + size_t(cx)]) return true;
      }
    return false;
  };
  std::vector<int> level(size_t(W) * size_t(L), INT32_MIN);  // per corner: the level of the street on it
  for (auto& part : parts) {
    std::vector<int> hs;
    for (auto [bx, bz] : part) hs.push_back(H(std::clamp(bx + 2, 0, W - 1), std::clamp(bz + 2, 0, L - 1)));
    std::nth_element(hs.begin(), hs.begin() + std::ptrdiff_t(hs.size() / 2), hs.end());
    int lv = (hs[hs.size() / 2] + 0x40) & ~0x7f;
    for (auto [bx, bz] : part)  // the block and a margin a wide piece may reach
      for (int z = std::max(0, bz - 2); z <= std::min(L - 1, bz + 6); ++z)
        for (int x = std::max(0, bx - 2); x <= std::min(W - 1, bx + 6); ++x) level[size_t(z) * size_t(W) + size_t(x)] = lv;
  }
  // the levelled corners, then the ease over 4 cells around them (the nearest level wins)
  std::vector<int16_t> out(g.height);
  const int R = 4;
  for (int z = 0; z < L; ++z)
    for (int x = 0; x < W; ++x) {
      size_t i = size_t(z) * size_t(W) + size_t(x);
      if (wetCorner(x, z)) continue;
      if (level[i] != INT32_MIN) {
        out[i] = int16_t(level[i]);
        continue;
      }
      int best = R + 1, lv = 0;
      for (int dz = -R; dz <= R; ++dz)
        for (int dx = -R; dx <= R; ++dx) {
          int nx = x + dx, nz = z + dz;
          if (nx < 0 || nz < 0 || nx >= W || nz >= L) continue;
          int l2 = level[size_t(nz) * size_t(W) + size_t(nx)];
          int dd = std::max(std::abs(dx), std::abs(dz));
          if (l2 != INT32_MIN && dd < best) best = dd, lv = l2;
        }
      if (best <= R) out[i] = int16_t(lv + (out[i] - lv) * best / (R + 1));
    }
  g.height = out;
  heights = out;
}

struct Candidate {
  std::string name;
  int w, l;
};

std::vector<Candidate> categoryTemplates(VM& vm, GridObj* grid, const Value& buildings, const std::string& cat) {
  std::vector<Candidate> out;
  MapObj* m = vm.resolve(buildings).map();
  if (!m) return out;
  for (auto& [k, v] : m->entries) {
    if (utf16ToUtf8(vm.toStr(k)) != cat) continue;
    ArrayObj* a = vm.resolve(v).array();
    if (!a) continue;
    for (const Value& n : a->items) {
      std::string name = utf16ToUtf8(vm.toStr(n));
      auto it = grid->templates.find(name);
      if (it == grid->templates.end()) continue;
      Object* o = it->second.get();
      auto f = [&](const char16_t* key) {
        auto fv = o->fieldValues.find(key);
        return fv == o->fieldValues.end() || fv->second.isNone() ? 1 : std::max(1, int(vm.toInt(fv->second)));
      };
      out.push_back({name, f(u"width"), f(u"length")});
    }
  }
  return out;
}

int maxCorner(const GridData& g, int x, int z, int w, int l) {
  int hi = -0x8000;
  for (int zz = z; zz <= std::min(z + l, g.length - 1); ++zz)
    for (int xx = x; xx <= std::min(x + w, g.width - 1); ++xx) hi = std::max(hi, int(g.height[size_t(zz) * size_t(g.width) + size_t(xx)]));
  return hi;
}
int meanCorner(const GridData& g, int x, int z) {
  long s = 0;
  int n = 0;
  for (int dz = 0; dz <= 1; ++dz)
    for (int dx = 0; dx <= 1; ++dx)
      if (x + dx < g.width && z + dz < g.length) s += g.height[size_t(z + dz) * size_t(g.width) + size_t(x + dx)], ++n;
  return n ? int(s / n) : 0x200;
}

int addItem(VM& vm, GridObj* grid, Apply& a, const std::string& name, int x, int y, int z, int orient) {
  int i = gridEditAddItem(vm, grid, name, x, y, z, orient, 0, -1, false);
  if (i >= 0) a.made.push_back(i);
  return i;
}

// front orientation of a building facing side s (0 north −z, 1 east +x, 2 south +z, 3 west −x); env REGS_OSM_FRONT
// overrides the table for checks ("0123")
int frontOrient(int side) {
  static int table[4] = {2, 1, 0, 3};
  static bool init = false;
  if (!init) {
    init = true;
    if (const char* e = std::getenv("REGS_OSM_FRONT"); e && std::strlen(e) == 4)
      for (int i = 0; i < 4; ++i) table[i] = e[i] - '0';
  }
  return table[side & 3];
}

void placeBuilding(VM& vm, GridObj* grid, Apply& a, const BuildingIn& b, const Value& buildingsList,
                   std::map<std::string, std::vector<Candidate>>& cache) {
  GridData& g = *grid->data;
  const int W = g.width, L = g.length;
  if (b.pts.size() < 3) return;
  // the minimum-area rectangle over the hull's edge directions
  auto pts = b.pts;
  std::sort(pts.begin(), pts.end());
  pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
  if (pts.size() < 3) return;
  std::vector<std::pair<double, double>> hull;
  auto cross = [](std::pair<double, double> o, std::pair<double, double> p, std::pair<double, double> q) {
    return (p.first - o.first) * (q.second - o.second) - (p.second - o.second) * (q.first - o.first);
  };
  for (int pass = 0; pass < 2; ++pass) {
    size_t start = hull.size();
    for (size_t k = 0; k < pts.size(); ++k) {
      auto p = pass ? pts[pts.size() - 1 - k] : pts[k];
      while (hull.size() >= start + 2 && cross(hull[hull.size() - 2], hull.back(), p) <= 0) hull.pop_back();
      hull.push_back(p);
    }
    hull.pop_back();
  }
  if (hull.size() < 3) return;
  double bestArea = 1e30, ang = 0, ea = 0, eb = 0, ccx = 0, ccz = 0;
  for (size_t i = 0; i < hull.size(); ++i) {
    auto p = hull[i], q = hull[(i + 1) % hull.size()];
    double t = std::atan2(q.second - p.second, q.first - p.first);
    double c = std::cos(t), s = std::sin(t);
    double u0 = 1e30, u1 = -1e30, v0 = 1e30, v1 = -1e30;
    for (auto& h : hull) {
      double u = h.first * c + h.second * s, v = -h.first * s + h.second * c;
      u0 = std::min(u0, u), u1 = std::max(u1, u), v0 = std::min(v0, v), v1 = std::max(v1, v);
    }
    double area = (u1 - u0) * (v1 - v0);
    if (area < bestArea) {
      bestArea = area, ang = t, ea = u1 - u0, eb = v1 - v0;
      double uc = (u0 + u1) * 0.5, vc = (v0 + v1) * 0.5;
      ccx = uc * c - vc * s, ccz = uc * s + vc * c;
    }
  }
  // the rectangle laid on the cell axes: its sides along x and z
  double deg = std::fmod(std::abs(ang * 180.0 / kPi), 180.0);
  bool swap = deg > 45.0 && deg < 135.0;
  double ex = swap ? eb : ea, ez = swap ? ea : eb;
  if (ex < 0.8 || ez < 0.8) return;
  // candidates of the category (or its neighbours)
  static const std::map<std::string, std::vector<std::string>> kFallback = {
      {"residential", {"residential", "blockhouses"}}, {"blockhouses", {"blockhouses", "residential"}},
      {"commercial", {"commercial", "workplace", "blockhouses"}}, {"workplace", {"workplace", "commercial"}},
      {"industry", {"industry", "workplace"}}, {"government", {"government", "workplace", "commercial"}},
      {"farm", {"farm", "residential"}}, {"railway", {"railway", "industry"}}};
  std::vector<Candidate> cands;
  auto fb = kFallback.find(b.category);
  for (const std::string& c : fb == kFallback.end() ? std::vector<std::string>{b.category, "residential"} : fb->second) {
    auto it = cache.find(c);
    if (it == cache.end()) it = cache.emplace(c, categoryTemplates(vm, grid, buildingsList, c)).first;
    if (!it->second.empty()) {
      cands = it->second;
      break;
    }
  }
  if (cands.empty()) return;
  struct Fit {
    const Candidate* c;
    int sx, sz, orient2;  // orient2: 0 or 1 (the template turned by 90°)
    double score;
  };
  std::vector<Fit> fits;
  const Candidate* smallest = nullptr;
  for (const Candidate& c : cands) {
    if (!smallest || c.w * c.l < smallest->w * smallest->l) smallest = &c;
    for (int o = 0; o < 2; ++o) {
      int sx = o ? c.l : c.w, sz = o ? c.w : c.l;
      if (sx > ex + 0.6 || sz > ez + 0.6) continue;
      fits.push_back({&c, sx, sz, o, double(sx * sz) / (ex * ez)});
    }
  }
  double best = 0;
  for (auto& f : fits) best = std::max(best, f.score);
  std::vector<const Fit*> good;
  for (auto& f : fits)
    if (f.score >= best * 0.85) good.push_back(&f);
  Fit chosen{};
  if (good.empty()) {
    if (!smallest || smallest->w * smallest->l > ex * ez * 1.8) return;
    bool o = (smallest->w > smallest->l) != (ex > ez);
    chosen = {smallest, o ? smallest->l : smallest->w, o ? smallest->w : smallest->l, o ? 1 : 0, 1.0};
  } else {
    chosen = *good[hash32(b.id) % good.size()];
  }
  // a big footprint gets several buildings in a row or grid inside it
  int nx = 1, nz = 1;
  if (chosen.score < 0.45 && b.polygon) {
    nx = std::max(1, int(ex / chosen.sx)), nz = std::max(1, int(ez / chosen.sz));
    while (nx * nz > 16) nx > nz ? --nx : --nz;
  }
  auto insidePoly = [&](double x, double z) {
    if (!b.polygon) return true;
    int n = 0;
    for (size_t i = 0, j = b.pts.size() - 1; i < b.pts.size(); j = i++) {
      auto p = b.pts[i], q = b.pts[j];
      if ((p.second <= z) != (q.second <= z) && x < p.first + (z - p.second) * (q.first - p.first) / (q.second - p.second)) ++n;
    }
    return (n & 1) != 0;
  };
  auto freeRect = [&](int x, int z, int w, int l) {
    if (x < 1 || z < 1 || x + w >= W - 1 || z + l >= L - 1) return false;
    for (int zz = z; zz < z + l; ++zz)
      for (int xx = x; xx < x + w; ++xx)
        if (a.used[size_t(zz) * size_t(W) + size_t(xx)]) return false;
    return true;
  };
  auto roadAt = [&](int x, int z) {
    return x >= 0 && z >= 0 && x < W && z < L && a.used[size_t(z) * size_t(W) + size_t(x)] == 2;
  };
  double stepX = ex / nx, stepZ = ez / nz;
  for (int iz = 0; iz < nz; ++iz)
    for (int ix = 0; ix < nx; ++ix) {
      double px = ccx - ex * 0.5 + stepX * (ix + 0.5), pz = ccz - ez * 0.5 + stepZ * (iz + 0.5);
      if (nx * nz > 1 && !insidePoly(px, pz)) continue;
      const Candidate& c = *chosen.c;
      int sx = chosen.sx, sz = chosen.sz;
      int bx = int(std::lround(px - sx * 0.5)), bz = int(std::lround(pz - sz * 0.5));
      bool placed = false;
      for (int r = 0; r <= 2 && !placed; ++r)
        for (int dz = -r; dz <= r && !placed; ++dz)
          for (int dx = -r; dx <= r && !placed; ++dx) {
            if (std::max(std::abs(dx), std::abs(dz)) != r) continue;
            int x = bx + dx, z = bz + dz;
            if (!freeRect(x, z, sx, sz)) continue;
            // the front towards the nearest road: north, east, south or west side
            int dist[4] = {99, 99, 99, 99};
            for (int k = 1; k <= 4; ++k) {
              for (int t = 0; t < sx; ++t) {
                if (dist[0] == 99 && roadAt(x + t, z - k)) dist[0] = k;
                if (dist[2] == 99 && roadAt(x + t, z + sz - 1 + k)) dist[2] = k;
              }
              for (int t = 0; t < sz; ++t) {
                if (dist[3] == 99 && roadAt(x - k, z + t)) dist[3] = k;
                if (dist[1] == 99 && roadAt(x + sx - 1 + k, z + t)) dist[1] = k;
              }
            }
            // the sides the turned template can face: orient2 0 → north/south, 1 → east/west
            int sideA = chosen.orient2 ? 1 : 0, sideB = sideA + 2;
            int side = dist[sideB] < dist[sideA] ? sideB : dist[sideA] < dist[sideB] ? sideA : (hash32(b.id + 7) & 1 ? sideA : sideB);
            int orient = frontOrient(side);
            if ((orient & 1) != chosen.orient2) orient = (orient + 1) & 3;  // the table must keep the footprint
            int y = maxCorner(g, x, z, sx, sz);
            if (addItem(vm, grid, a, c.name, x, y, z, orient) >= 0) {
              for (int zz = z; zz < z + sz; ++zz)
                for (int xx = x; xx < x + sx; ++xx) a.used[size_t(zz) * size_t(W) + size_t(xx)] = 3;
              ++a.buildingsPlaced;
              placed = true;
            }
          }
    }
}

// one slice of the import's application; true when finished
bool applySlice(VM& vm, GridObj* grid, ControlObj* player, const Value& buildings, const Value& trees, double budget) {
  Apply& a = *g_apply;
  ImportData& d = *a.d;
  GridData& g = *grid->data;
  const int W = g.width, L = g.length;
  double t0 = now();
  auto setP = [&](int step, float p) {
    std::lock_guard<std::mutex> lk(g_import.mx);
    g_import.state = 4;
    g_import.step = step;
    g_import.progress = p;
  };
  if (d.W != W || d.L != L) {
    std::lock_guard<std::mutex> lk(g_import.mx);
    g_import.state = 3;
    g_import.error = "size";
    return true;
  }
  if (a.stage == 0) {  // begin: the group, the old objects
    a.timeline = playerTimeline(vm, player);
    if (a.timeline) timelineBeginGroup(vm, a.timeline, "%9 OpenStreetMap");
    a.used.assign(size_t(W) * size_t(L), 0);
    if (d.flags & kClear) {
      gridEditBegin(vm, grid);
      for (uint32_t i = 0; i < g.items.size(); ++i) {
        if (g.items[i].removed || g.items[i].parent) continue;
        timelineRecordDelete(vm, a.timeline, grid, i);
        gridEditRemoveItem(vm, grid, i);
      }
      gridEditEnd(vm, grid);
    }
    a.stage = 1;
  }
  if (a.stage == 1) {  // terrain and water: one patch over the whole map
    setP(5, 0);
    buildNetwork(d.roads, W, L, d.water, a.network, a.drags);
    bool water = (d.flags & kWater) != 0;
    if (!d.heights.empty() || water) {
      std::vector<int16_t> heights(size_t(W) * size_t(L));
      std::vector<uint16_t> flags(heights.size());
      constexpr uint16_t kGround = 0x203F;
      for (int z = 0; z < L; ++z)
        for (int x = 0; x < W; ++x) {
          size_t i = size_t(z) * size_t(W) + size_t(x);
          int h = d.heights.empty() ? g.height[i] : d.heights[i];
          // a corner of a water cell lies under the water line
          bool wet = false;
          for (int dz = -1; dz <= 0 && water; ++dz)
            for (int dx = -1; dx <= 0; ++dx) {
              int cx = x + dx, cz = z + dz;
              if (cx >= 0 && cz >= 0 && cx < W && cz < L && d.water[size_t(cz) * size_t(W) + size_t(cx)]) wet = true;
            }
          if (wet) h = std::min(h, -0x300);
          g.height[i] = int16_t(h);
          heights[i] = int16_t(h);
          uint16_t f = g.flags[i];
          if (water) f = uint16_t(d.water[i] ? (f & ~0x3Fu) | 0x2000 : f & ~0x2000u);
          if (!d.heights.empty() && !d.water[i]) f &= uint16_t(~0x3Fu & 0xFFFF);  // new ground: plain grass
          g.flags[i] = f;
          flags[i] = uint16_t(f & kGround);
          if (d.water[i]) a.used[i] = 1;
        }
      if (!d.heights.empty()) levelUnderRoads(g, a.network, d.water, heights);
      gridEditCellsChanged(grid, 0, 0, W - 1, L - 1, true);
      if (a.timeline) timelineRecordPatch(vm, a.timeline, 0, 0, W, L, heights, flags);
    }
    a.stage = 2;
    a.next = 0;
    if (now() - t0 > budget) return false;
  }
  if (a.stage == 2) {  // the road network at once, then the alleys a drag at a time
    if (!a.network.empty()) {
      std::vector<uint32_t> mounted;
      size_t wanted = a.network.size();
      a.roadsOk = toolBuildRoadNetwork(vm, player, grid, std::move(a.network), mounted);
      a.roadsFailed = int(wanted) - a.roadsOk;
      for (uint32_t m : mounted) a.made.push_back(int(m));
      a.network.clear();
      if (now() - t0 > budget) {
        setP(6, 0.5f);
        return false;
      }
    }
    while (a.next < a.drags.size()) {
      const Drag& dr = a.drags[a.next++];
      toolLayRoad(vm, player, grid, dr.editMode, dr.toolMode, dr.x0, dr.z0, dr.x1, dr.z1);
      if (now() - t0 > budget) {
        setP(6, float(a.next) / float(a.drags.size()));
        return false;
      }
    }
    // what the streets took (every item on the map now)
    for (uint32_t i = 0; i < g.items.size(); ++i) {
      if (g.items[i].removed) continue;
      int x, z, w, l;
      gridItemRect(vm, grid, i, x, z, w, l);
      for (int zz = std::max(0, z); zz < std::min(L, z + l); ++zz)
        for (int xx = std::max(0, x); xx < std::min(W, x + w); ++xx) a.used[size_t(zz) * size_t(W) + size_t(xx)] = 2;
    }
    a.stage = 3;
    a.next = 0;
  }
  if (a.stage == 3) {  // buildings
    static std::map<std::string, std::vector<Candidate>> cache;
    if (a.next == 0) cache.clear();
    gridEditBegin(vm, grid);
    while (a.next < d.buildings.size()) {
      placeBuilding(vm, grid, a, d.buildings[a.next++], buildings, cache);
      if (now() - t0 > budget) break;
    }
    gridEditEnd(vm, grid);
    if (a.next < d.buildings.size()) {
      setP(7, float(a.next) / float(d.buildings.size()));
      return false;
    }
    a.stage = 4;
    a.next = 0;
  }
  if (a.stage == 4) {  // trees
    std::vector<std::string> names;
    if (ArrayObj* arr = vm.resolve(trees).array())
      for (const Value& v : arr->items) {
        std::string n = utf16ToUtf8(vm.toStr(v));
        if (grid->templates.count(n)) names.push_back(n);
      }
    gridEditBegin(vm, grid);
    while (!names.empty() && a.next < d.trees.size()) {
      auto [x, z] = d.trees[a.next++];
      size_t i = size_t(z) * size_t(W) + size_t(x);
      if (x < 1 || z < 1 || x >= W - 1 || z >= L - 1 || a.used[i]) continue;
      uint32_t h = hash32((uint64_t(x) << 32) | uint32_t(z));
      if (addItem(vm, grid, a, names[h % names.size()], x, meanCorner(g, x, z), z, int((h >> 8) & 3)) >= 0) {
        a.used[i] = 4;
        ++a.treesPlaced;
      }
      if (now() - t0 > budget) break;
    }
    gridEditEnd(vm, grid);
    if (!names.empty() && a.next < d.trees.size()) {
      setP(8, float(a.next) / float(d.trees.size()));
      return false;
    }
    a.stage = 5;
  }
  // done: the new objects on the timeline, their events, the overlay picture
  for (int i : a.made)
    if (size_t(i) < g.items.size() && !g.items[size_t(i)].removed) timelineRecordCreate(vm, a.timeline, grid, uint32_t(i));
  if (a.timeline) timelineEndGroup(vm, a.timeline);
  for (int i : a.made) {
    if (size_t(i) >= g.items.size() || g.items[size_t(i)].removed || g.items[size_t(i)].parent) continue;
    auto t = grid->templates.find(g.types[g.items[size_t(i)].type]);
    Object* c = gridItemController(vm, grid, uint32_t(i));
    if (t == grid->templates.end() || !c) continue;
    try {
      vm.fireEvent(t->second.get(), u"onCreateInstance", {Value::heap(VT::Object, c)});
    } catch (const std::exception& e) {
      LOG_ERROR("onCreateInstance: %s", e.what());
    }
  }
  if (!d.satellite.empty()) {
    GroundOverlay& o = groundOverlay();
    o.bgra = d.satellite;
    o.width = d.satW, o.height = d.satH;
    o.visible = true;
    ++o.version;
  }
  LOG_INFO("osm: applied: %d road pieces, %d blocks without, %d buildings, %d trees", a.roadsOk, a.roadsFailed, a.buildingsPlaced,
           a.treesPlaced);
  {
    std::lock_guard<std::mutex> lk(g_import.mx);
    g_import.state = 5;
    g_import.step = 0;
    g_import.progress = 1;
  }
  return true;
}

Value floats(std::initializer_list<double> v) {
  auto a = make<ArrayObj>();
  for (double x : v) a->items.push_back(Value::real(float(x)));
  return Value::heap(VT::Array, a.get());
}

}  // namespace

void registerOsmNatives(VM& vm) {
  // regsOsmView(texture, w, h): the map view into the texture; true while tiles are still loading
  implementGlobal(vm, u"regsOsmView", [](VM& v, Object*, int, const Value* a) {
    auto* tex = dynamic_cast<TextureObj*>(v.resolve(a[0]).object());
    int w = std::clamp(int(v.toInt(a[1])), 16, 4096), h = std::clamp(int(v.toInt(a[2])), 16, 4096);
    g_view.lastW = w, g_view.lastH = h;
    std::vector<uint8_t> px;
    bool loading = false;
    compose(px, w, h, loading);
    Display* d = display();
    if (!tex || !d) return Value::boolean(loading);
    if (tex->gl && tex->width == w && tex->height == h) d->r2d().updateTextureBGRA(tex->gl, px.data(), w, h);
    else {
      if (tex->gl) d->r2d().destroyTexture(tex->gl);
      tex->gl = d->r2d().createTextureBGRA(px.data(), w, h);
    }
    tex->width = w, tex->height = h;
    return Value::boolean(loading);
  }, 3, 3);
  // regsOsmTexture(): the view's own texture, made once (video.new_texture() without a path is the shared default
  // texture, which the minimap draws into; deleting it would break the minimap)
  implementGlobal(vm, u"regsOsmTexture", [](VM& v, Object*, int, const Value*) {
    static Ref<Object> tex;
    if (!tex) tex = v.newObject(v.findClass("gsTextureVariant_OGL"), nullptr);
    return Value::heap(VT::Object, tex.get());
  }, 0, 0);
  // regsOsmTilesVersion(): moves when a tile arrived (the window redraws then)
  implementGlobal(vm, u"regsOsmTilesVersion", [](VM&, Object*, int, const Value*) {
    return Value::integer(int32_t(store().version.load() & 0x7fffffff));
  }, 0, 0);
  implementGlobal(vm, u"regsOsmPan", [](VM& v, Object*, int, const Value* a) {
    g_view.placed = true;
    double S = worldSize(g_view.z);
    g_view.mx = std::fmod(g_view.mx - v.toFloat(a[0]) / S + 1.0, 1.0);
    g_view.my = std::clamp(g_view.my - v.toFloat(a[1]) / S, 0.0, 1.0);
    return Value();
  }, 2, 2);
  // regsOsmZoom(steps, px, py): zoom in (+) or out (−) keeping the point under (px, py) in place
  implementGlobal(vm, u"regsOsmZoom", [](VM& v, Object*, int, const Value* a) {
    g_view.placed = true;
    int nz = std::clamp(g_view.z + int(v.toInt(a[0])), 2, 19);
    double px = v.toFloat(a[1]) - g_view.lastW * 0.5, py = v.toFloat(a[2]) - g_view.lastH * 0.5;
    double S0 = worldSize(g_view.z), S1 = worldSize(nz);
    double wx = g_view.mx + px / S0, wy = g_view.my + py / S0;
    g_view.mx = wx - px / S1, g_view.my = std::clamp(wy - py / S1, 0.0, 1.0);
    g_view.z = nz;
    return Value::integer(nz);
  }, 3, 3);
  implementGlobal(vm, u"regsOsmLayer", [](VM& v, Object*, int argc, const Value* a) {
    if (argc > 0) g_view.layer = std::clamp(int(v.toInt(a[0])), 0, 1);
    return Value::integer(g_view.layer);
  }, 0, 1);
  implementGlobal(vm, u"regsOsmGoto", [](VM& v, Object*, int, const Value* a) {
    g_view.placed = true;
    g_view.mx = lonToMx(v.toFloat(a[1]));
    g_view.my = latToMy(v.toFloat(a[0]));
    g_view.z = std::clamp(int(v.toInt(a[2])), 2, 19);
    return Value();
  }, 3, 3);
  // regsOsmStartView(location): the first view of the session (owner's request 2026-11-07: zoomed in, near the
  // player): the main city of the game location chosen in the new-map dialog (its folder name), else the Windows
  // region's country; later calls keep where the player left the view
  implementGlobal(vm, u"regsOsmStartView", [](VM& v, Object*, int, const Value* a) {
    if (g_view.placed) return Value();
    static const std::map<std::string, std::array<double, 2>> kCities = {
        {"germany", {52.5200, 13.4050}},  {"austria", {48.2082, 16.3738}},   {"finland", {60.1699, 24.9384}},
        {"netherlands", {52.3676, 4.9041}}, {"holland", {52.3676, 4.9041}}, {"england", {51.5074, -0.1278}},
        {"london", {51.5074, -0.1278}},  {"france", {48.8566, 2.3522}},     {"paris", {48.8566, 2.3522}},
        {"russia", {59.9343, 30.3351}},  {"japan", {35.6762, 139.6503}},   {"tokyo", {35.6762, 139.6503}},
        {"california", {37.7749, -122.4194}}, {"newyork", {40.7128, -74.0060}}, {"ulm", {48.3984, 9.9916}},
        {"munich", {48.1374, 11.5755}},  {"berlin", {52.5200, 13.4050}}};
    std::string loc = utf16ToUtf8(v.toStr(a[0]));
    if (auto sl = loc.find_last_of("/\\"); sl != std::string::npos) loc = loc.substr(sl + 1);
    for (char& c : loc) c = char(std::tolower(uint8_t(c)));
    double lat = 0, lon = 0;
    int zoom = 12;
    if (auto it = kCities.find(loc); it != kCities.end()) lat = it->second[0], lon = it->second[1];
    else if (platformHomeRegion(lat, lon)) zoom = 6;
    else lat = 52.5200, lon = 13.4050, zoom = 5;
    g_view.mx = lonToMx(lon);
    g_view.my = latToMy(lat);
    g_view.z = zoom;
    g_view.placed = true;
    return Value();
  }, 1, 1);
  // regsOsmCenter(): [lat, lon, zoom] of the view
  implementGlobal(vm, u"regsOsmCenter", [](VM&, Object*, int, const Value*) {
    return floats({myToLat(g_view.my), mxToLon(g_view.mx), double(g_view.z)});
  }, 0, 0);
  // regsOsmSetArea(px, py): the rectangle's centre to view pixel (px, py)
  implementGlobal(vm, u"regsOsmSetArea", [](VM& v, Object*, int, const Value* a) {
    g_view.placed = true;
    double S = worldSize(g_view.z);
    g_view.amx = g_view.mx + (v.toFloat(a[0]) - g_view.lastW * 0.5) / S;
    g_view.amy = std::clamp(g_view.my + (v.toFloat(a[1]) - g_view.lastH * 0.5) / S, 0.01, 0.99);
    g_view.hasArea = true;
    return Value();
  }, 2, 2);
  // regsOsmMoveArea(dx, dy): the rectangle moved by view pixels
  implementGlobal(vm, u"regsOsmMoveArea", [](VM& v, Object*, int, const Value* a) {
    double S = worldSize(g_view.z);
    g_view.amx += v.toFloat(a[0]) / S;
    g_view.amy = std::clamp(g_view.amy + v.toFloat(a[1]) / S, 0.01, 0.99);
    return Value();
  }, 2, 2);
  // regsOsmHitArea(px, py): the view pixel is inside the rectangle
  implementGlobal(vm, u"regsOsmHitArea", [](VM& v, Object*, int, const Value* a) {
    if (!g_view.hasArea) return Value::boolean(false);
    double S = worldSize(g_view.z), aw, al;
    areaSize(aw, al);
    double x = g_view.mx + (v.toFloat(a[0]) - g_view.lastW * 0.5) / S, y = g_view.my + (v.toFloat(a[1]) - g_view.lastH * 0.5) / S;
    double m = 6.0 / S;  // a few pixels around small rectangles
    return Value::boolean(std::abs(x - g_view.amx) <= aw * 0.5 + m && std::abs(y - g_view.amy) <= al * 0.5 + m);
  }, 2, 2);
  // regsOsmAreaSize(cellsW, cellsL, scale)
  implementGlobal(vm, u"regsOsmAreaSize", [](VM& v, Object*, int, const Value* a) {
    g_view.cellsW = std::max(16, int(v.toInt(a[0])));
    g_view.cellsL = std::max(16, int(v.toInt(a[1])));
    g_view.scale = std::clamp(double(v.toFloat(a[2])), 0.25, 20.0);
    return Value();
  }, 3, 3);
  // regsOsmArea(): null without a rectangle, else [lat, lon, width in km, length in km]
  implementGlobal(vm, u"regsOsmArea", [](VM&, Object*, int, const Value*) {
    if (!g_view.hasArea) return Value::null();
    double m = metresPerUnit(g_view.amy), aw, al;
    areaSize(aw, al);
    return floats({myToLat(g_view.amy), mxToLon(g_view.amx), aw * m / 1000.0, al * m / 1000.0});
  }, 0, 0);
  implementGlobal(vm, u"regsOsmClearArea", [](VM&, Object*, int, const Value*) {
    g_view.hasArea = false;
    return Value();
  }, 0, 0);
  // regsOsmSearch(text, language): starts a search; regsOsmSearchResult(): null while running, then an array of
  // [name, lat, lon, zoom] (empty when nothing was found); regsOsmSearchState(): 0 idle, 1 running, 2 done, 3 failed
  implementGlobal(vm, u"regsOsmSearch", [](VM& v, Object*, int argc, const Value* a) {
    std::string text = utf16ToUtf8(v.toStr(a[0]));
    std::string lang = argc > 1 ? utf16ToUtf8(v.toStr(a[1])) : "en";
    if (lang.size() > 2) lang.resize(2);
    if (!text.empty()) startSearch(text, lang);
    return Value();
  }, 1, 2);
  implementGlobal(vm, u"regsOsmSearchState", [](VM&, Object*, int, const Value*) {
    std::lock_guard<std::mutex> lk(g_search.mx);
    return Value::integer(g_search.state);
  }, 0, 0);
  implementGlobal(vm, u"regsOsmSearchResult", [](VM&, Object*, int, const Value*) {
    std::lock_guard<std::mutex> lk(g_search.mx);
    if (g_search.state == 1) return Value::null();
    auto arr = make<ArrayObj>();
    for (auto& h : g_search.hits) {
      auto e = make<ArrayObj>();
      e->items.push_back(Value::string(utf8ToUtf16(h.name)));
      e->items.push_back(Value::real(float(h.lat)));
      e->items.push_back(Value::real(float(h.lon)));
      e->items.push_back(Value::integer(h.zoom));
      arr->items.push_back(Value::heap(VT::Array, e.get()));
    }
    return Value::heap(VT::Array, arr.get());
  }, 0, 0);
  // regsOsmImportStart(cellsW, cellsL, flags): downloads the rectangle's data (flags: 1 terrain, 2 water, 4 trees,
  // 8 streets, 16 buildings, 32 satellite picture, 64 remove the map's objects first)
  implementGlobal(vm, u"regsOsmImportStart", [](VM& v, Object*, int, const Value* a) {
    if (!g_view.hasArea) return Value::boolean(false);
    Frame fr;
    fr.W = int(v.toInt(a[0])), fr.L = int(v.toInt(a[1]));
    int flags = int(v.toInt(a[2]));
    fr.cmx = g_view.amx, fr.cmy = g_view.amy;
    fr.unitsPerCell = 3.0 * g_view.scale / metresPerUnit(g_view.amy);
    int serial;
    {
      std::lock_guard<std::mutex> lk(g_import.mx);
      if (g_import.state == 1 || g_import.state == 4) return Value::boolean(false);
      serial = ++g_import.serial;
      g_import.state = 1;
      g_import.step = 0;
      g_import.progress = 0;
      g_import.error.clear();
      g_import.data.reset();
      g_import.cancel = false;
    }
    std::thread([fr, flags, serial] {
      std::string err;
      std::shared_ptr<ImportData> data;
      try {
        runImport(serial, fr, flags, &err, data);
      } catch (const std::exception& e) {
        err = e.what();
      }
      std::lock_guard<std::mutex> lk(g_import.mx);
      if (serial != g_import.serial) return;
      if (g_import.cancel) g_import.state = 0;
      else if (!data) g_import.state = 3, g_import.error = err.empty() ? "failed" : err;
      else g_import.state = 2, g_import.data = data;
    }).detach();
    return Value::boolean(true);
  }, 3, 3);
  implementGlobal(vm, u"regsOsmImportCancel", [](VM&, Object*, int, const Value*) {
    g_import.cancel = true;
    std::lock_guard<std::mutex> lk(g_import.mx);
    if (g_import.state != 4) g_import.state = 0, ++g_import.serial;
    return Value();
  }, 0, 0);
  // regsOsmImportStatus(): [state, step, progress, error]; state 0 idle, 1 downloading, 2 ready, 3 failed, 4 applying,
  // 5 done; step 1 elevation, 2 map data, 3 satellite picture, 4 preparing, 5 terrain, 6 streets, 7 buildings, 8 trees
  implementGlobal(vm, u"regsOsmImportStatus", [](VM&, Object*, int, const Value*) {
    std::lock_guard<std::mutex> lk(g_import.mx);
    auto arr = make<ArrayObj>();
    arr->items.push_back(Value::integer(g_import.state));
    arr->items.push_back(Value::integer(g_import.step));
    arr->items.push_back(Value::real(g_import.progress));
    arr->items.push_back(Value::string(utf8ToUtf16(g_import.error)));
    return Value::heap(VT::Array, arr.get());
  }, 0, 0);
  // grid.regsOsmApply(player, buildings, trees, budgetSeconds): applies the downloaded import, a slice per call;
  // true when finished (MAIN.$level.$buildings and .$trees give the game objects to use)
  implement(vm, "gsGrid", u"regsOsmApply", [](VM& v, Object* s, int argc, const Value* a) {
    auto* grid = dynamic_cast<GridObj*>(s);
    auto* player = dynamic_cast<ControlObj*>(v.resolve(a[0]).object());
    if (!grid || !grid->data || !player) return Value::boolean(true);
    if (!g_apply) {
      std::lock_guard<std::mutex> lk(g_import.mx);
      if (g_import.state != 2 || !g_import.data) return Value::boolean(true);
      g_apply = std::make_unique<Apply>();
      g_apply->d = g_import.data;
      g_import.state = 4;
    }
    double budget = argc > 3 ? v.toFloat(a[3]) : 0.03;
    bool done = applySlice(v, grid, player, a[1], a[2], budget);
    if (done) g_apply.reset();
    return Value::boolean(done);
  });
  // the satellite picture on the ground: regsOsmOverlay() → 0 none, 1 hidden, 2 shown; regsOsmShowOverlay(on)
  implementGlobal(vm, u"regsOsmOverlay", [](VM&, Object*, int, const Value*) {
    GroundOverlay& o = groundOverlay();
    return Value::integer(o.bgra.empty() ? 0 : o.visible ? 2 : 1);
  }, 0, 0);
  implementGlobal(vm, u"regsOsmShowOverlay", [](VM& v, Object*, int, const Value* a) {
    GroundOverlay& o = groundOverlay();
    bool on = v.truthy(a[0]);
    if (on != o.visible) o.visible = on, ++o.version;
    return Value();
  }, 1, 1);
}

}  // namespace regs::engine
