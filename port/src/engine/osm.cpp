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
#include "engine/curve_roads.h"
#include "engine/display.h"
#include "engine/grid.h"
#include "engine/grid_edit.h"
#include "engine/gui.h"
#include "engine/host.h"
#include "engine/interaction.h"
#include "engine/platform.h"
#include "engine/timeline.h"
#include "engine/tool_tracks.h"
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
  int kind = 0;  // 0 small road, 1 large road (multi-lane), 2 alley, 3 avenue (trees in the median), 4 express road,
                 // 5 railway (double track)
  bool bridge = false, oneway = false, treeLined = false, dropped = false;
  bool ring = false;  // junction=roundabout
  bool underpass = false;  // a short tunnel (under a railway or road): on the ground, what crosses it bridges it
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
  // the merged carriageways' points moved to the centre line: the streets ending on them follow (they stay joined)
  auto key = [](std::pair<double, double> q) { return std::pair<long long, long long>{std::llround(q.first * 64), std::llround(q.second * 64)}; };
  std::map<std::pair<long long, long long>, std::pair<double, double>> moved;
  int pairsMerged = 0;
  std::set<const RoadIn*> centred;  // ways made a centre line (no carriageway any more)
  // the distance of q to a way where q lies beside it (its foot not beyond the way's ends)
  auto beside = [](std::pair<double, double> q, const RoadIn& w, double& dist) {
    dist = 1e30;
    bool in = false;
    for (size_t i = 0; i + 1 < w.pts.size(); ++i) {
      auto a = w.pts[i], b = w.pts[i + 1];
      double dx = b.first - a.first, dz = b.second - a.second, l2 = dx * dx + dz * dz;
      double tr = l2 > 0 ? ((q.first - a.first) * dx + (q.second - a.second) * dz) / l2 : 0, t = std::clamp(tr, 0.0, 1.0);
      double d = std::hypot(a.first + t * dx - q.first, a.second + t * dz - q.second);
      if (d < dist) dist = d, in = !((i == 0 && tr < 0) || (i + 2 == w.pts.size() && tr > 1));
    }
    return in;
  };
  for (auto& [name, group] : byName)
    for (RoadIn* a : group) {
      if (a->dropped) continue;
      auto da = dir(*a);
      std::vector<const RoadIn*> partners;
      // the other carriageway: ways running the other way beside this one (a carriageway another pair took already
      // still counts: the two sides are split into ways at different places)
      for (RoadIn* b : group) {
        if (b == a || centred.count(b)) continue;
        auto db = dir(*b);
        if (da.first * db.first + da.second * db.second > -0.5) continue;  // runs the same way
        std::vector<double> ds;
        double d;
        for (auto& q : b->pts)
          if (beside(q, *a, d)) ds.push_back(d);
        for (auto& q : a->pts)
          if (beside(q, *b, d)) ds.push_back(d);
        if (ds.empty()) continue;
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
        auto known = moved.find(key(q));
        if (known != moved.end()) {  // a point an earlier pair already moved (the ways of one road meet there)
          mid.push_back(known->second);
          if (d <= maxGap) gapSum += d;
          continue;
        }
        if (d > maxGap) {
          mid.push_back(q);
          continue;
        }
        mid.push_back({(q.first + o.first) * 0.5, (q.second + o.second) * 0.5});
        gapSum += d;
      }
      for (size_t i = 0; i < a->pts.size(); ++i) moved.emplace(key(a->pts[i]), mid[i]);
      // the partners' points onto the centre line (a point of it there, or a new one)
      {
        struct Ins {
          size_t seg;
          double t;
          std::pair<double, double> p;
        };
        std::vector<Ins> ins;
        for (const RoadIn* b : partners)
          for (auto& q : b->pts) {
            if (moved.count(key(q))) continue;
            double best = 1e30, bt = 0;
            size_t bs = 0;
            std::pair<double, double> bp;
            for (size_t i = 0; i + 1 < mid.size(); ++i) {
              double dx = mid[i + 1].first - mid[i].first, dz = mid[i + 1].second - mid[i].second, l2 = dx * dx + dz * dz;
              double t = l2 > 0 ? std::clamp(((q.first - mid[i].first) * dx + (q.second - mid[i].second) * dz) / l2, 0.0, 1.0) : 0;
              std::pair<double, double> o{mid[i].first + t * dx, mid[i].second + t * dz};
              double d = std::hypot(o.first - q.first, o.second - q.second);
              if (d < best) best = d, bt = t, bs = i, bp = o;
            }
            if (best > maxGap) continue;
            double segLen = std::hypot(mid[bs + 1].first - mid[bs].first, mid[bs + 1].second - mid[bs].second);
            if (bt * segLen < 0.3) moved[key(q)] = mid[bs];
            else if ((1 - bt) * segLen < 0.3) moved[key(q)] = mid[bs + 1];
            else ins.push_back({bs, bt, bp}), moved[key(q)] = bp;
          }
        std::sort(ins.begin(), ins.end(), [](const Ins& x, const Ins& y) { return x.seg != y.seg ? x.seg < y.seg : x.t < y.t; });
        std::vector<std::pair<double, double>> m2;
        size_t k = 0;
        for (size_t i = 0; i < mid.size(); ++i) {
          m2.push_back(mid[i]);
          while (k < ins.size() && ins[k].seg == i) {
            if (m2.back() != ins[k].p) m2.push_back(ins[k].p);
            ++k;
          }
        }
        mid.swap(m2);
      }
      ++pairsMerged;
      centred.insert(a);
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
      // lanes per carriageway (owner 2026-11-13: real lane counts): 2+ a side a large road (an avenue with the tree row),
      // one a side the small road on the centre line
      int side = std::max(a->lanes, 0);
      for (const RoadIn* b : partners) side = std::max(side, b->lanes);
      bool wide = side >= 2 || (side == 0 && a->cls >= 3);
      a->kind = a->cls >= 4 ? 4 : !wide ? 0 : trees ? 3 : 1;
      for (const RoadIn* b : partners) const_cast<RoadIn*>(b)->dropped = true;
    }
  // points of dropped carriageways no centre line took (beyond the part its pair covered): the streets ending there
  // join the nearest centre line
  {
    std::set<std::pair<long long, long long>> gone;
    for (RoadIn& r : roads)
      if (r.dropped)
        for (auto& q : r.pts) gone.insert(key(q));
    for (RoadIn& r : roads) {
      if (r.dropped || centred.count(&r)) continue;
      for (auto& q : r.pts) {
        auto k = key(q);
        if (!gone.count(k) || moved.count(k)) continue;
        RoadIn* best = nullptr;
        double bd = maxGap, bt = 0;
        size_t bs = 0;
        for (const RoadIn* cp : centred) {
          RoadIn* c = const_cast<RoadIn*>(cp);
          for (size_t i = 0; i + 1 < c->pts.size(); ++i) {
            auto a = c->pts[i], b = c->pts[i + 1];
            double dx = b.first - a.first, dz = b.second - a.second, l2 = dx * dx + dz * dz;
            double t = l2 > 0 ? std::clamp(((q.first - a.first) * dx + (q.second - a.second) * dz) / l2, 0.0, 1.0) : 0;
            double d = std::hypot(a.first + t * dx - q.first, a.second + t * dz - q.second);
            if (d < bd) bd = d, best = c, bs = i, bt = t;
          }
        }
        if (!best) continue;
        auto a = best->pts[bs], b = best->pts[bs + 1];
        double segLen = std::hypot(b.first - a.first, b.second - a.second);
        if (bt * segLen < 0.3) moved[k] = a;
        else if ((1 - bt) * segLen < 0.3) moved[k] = b;
        else {
          std::pair<double, double> o{a.first + bt * (b.first - a.first), a.second + bt * (b.second - a.second)};
          best->pts.insert(best->pts.begin() + std::ptrdiff_t(bs + 1), o);
          moved[k] = o;
        }
      }
    }
  }
  // the streets that ended on a carriageway end on the centre line
  for (RoadIn& r : roads) {
    if (r.dropped) continue;
    bool any = false;
    for (auto& q : r.pts) {
      auto it = moved.find(key(q));
      if (it != moved.end() && it->second != q) q = it->second, any = true;
    }
    if (!any) continue;
    std::vector<std::pair<double, double>> pts;
    for (auto& q : r.pts)
      if (pts.empty() || std::hypot(q.first - pts.back().first, q.second - pts.back().second) > 0.05) pts.push_back(q);
    r.pts.swap(pts);
  }
  if (std::getenv("REGS_DEBUG_OSM")) {
    LOG_INFO("osm: %d divided roads merged, %zu points moved", pairsMerged, moved.size());
    for (RoadIn& r : roads)
      if (!r.dropped && r.oneway && r.kind != 2 && r.cls >= 1)
        LOG_INFO("osm: one-way left: '%s' cls %d lanes %d, %zu points", r.name.c_str(), r.cls, r.lanes, r.pts.size());
  }
  for (RoadIn& r : roads) {
    if (r.dropped || r.kind != 0) continue;
    if (r.cls >= 4) r.kind = 4;
    // a carriageway without its partner: a one-way street (two of them side by side are what the map has)
    else if (r.oneway) r.lanes = std::max(r.lanes, r.cls >= 1 ? 2 : r.lanes);
    else {
      // the real lane count decides (owner 2026-11-13: lanes=2 or 3, one a side plus a turning lane, is a small road);
      // without one primary roads and 14 m+ are wide; a wide tree-lined road is an avenue
      bool wide = r.lanes > 0 ? r.lanes >= 4 : (r.width >= 14.0 || r.cls >= 3);
      if (wide) r.kind = r.treeLined ? 3 : 1;
    }
  }
  // one street, one kind (owner 2026-11-13: the changes between wide and narrow did not fit): the kind most of a named
  // street's length has (two-way small, large and avenue; one-way streets and footpaths keep theirs)
  {
    std::map<std::string, std::map<int, double>> len;
    auto two = [](const RoadIn& r) { return !r.dropped && !r.oneway && !r.ring && (r.kind == 0 || r.kind == 1 || r.kind == 3); };
    for (const RoadIn& r : roads)
      if (two(r) && !r.name.empty())
        for (size_t i = 0; i + 1 < r.pts.size(); ++i)
          len[r.name][r.kind] += std::hypot(r.pts[i + 1].first - r.pts[i].first, r.pts[i + 1].second - r.pts[i].second);
    for (RoadIn& r : roads) {
      if (!two(r) || r.name.empty()) continue;
      const auto& m = len[r.name];
      int best = r.kind;
      for (auto& [k, l] : m)
        if (l > m.at(best)) best = k;
      r.kind = best;
    }
  }
  roads.erase(std::remove_if(roads.begin(), roads.end(), [](const RoadIn& r) { return r.dropped; }), roads.end());
}
// parallel railway tracks closer than the game's double track is wide (4.5 cells) become one double track on their
// centre line (OSM maps every single track; each as a double track they would overlap); tracks ending on a merged
// one's points follow onto the centre line
void mergeTracks(std::vector<RoadIn>& roads) {
  const double maxGap = 4.5;
  auto key = [](std::pair<double, double> q) { return std::pair<long long, long long>{std::llround(q.first * 64), std::llround(q.second * 64)}; };
  std::map<std::pair<long long, long long>, std::pair<double, double>> moved;
  auto beside = [](std::pair<double, double> q, const RoadIn& w, double& dist, std::pair<double, double>& foot) {
    dist = 1e30;
    bool in = false;
    for (size_t i = 0; i + 1 < w.pts.size(); ++i) {
      auto a = w.pts[i], b = w.pts[i + 1];
      double dx = b.first - a.first, dz = b.second - a.second, l2 = dx * dx + dz * dz;
      double tr = l2 > 0 ? ((q.first - a.first) * dx + (q.second - a.second) * dz) / l2 : 0, t = std::clamp(tr, 0.0, 1.0);
      std::pair<double, double> f{a.first + t * dx, a.second + t * dz};
      double d = std::hypot(f.first - q.first, f.second - q.second);
      if (d < dist) dist = d, foot = f, in = !((i == 0 && tr < 0) || (i + 2 == w.pts.size() && tr > 1));
    }
    return in;
  };
  int merged = 0;
  for (RoadIn& a : roads) {
    if (a.kind != 5 || a.dropped || a.pts.size() < 2) continue;
    for (RoadIn& b : roads) {
      if (&b == &a || b.kind != 5 || b.dropped || b.pts.size() < 2) continue;
      // most of b lies beside a, closer than the double track is wide
      int near = 0, all = 0;
      double d;
      std::pair<double, double> f;
      for (auto& q : b.pts) {
        ++all;
        if (beside(q, a, d, f) && d < maxGap) ++near;
      }
      if (near * 3 < all * 2) continue;
      // a's points move halfway to b; b goes, its points onto a's new line
      for (auto& q : a.pts) {
        double db;
        std::pair<double, double> fb;
        if (beside(q, b, db, fb) && db < maxGap) {
          std::pair<double, double> m{(q.first + fb.first) * 0.5, (q.second + fb.second) * 0.5};
          moved[key(q)] = m;
          q = m;
        }
      }
      for (auto& q : b.pts) {
        beside(q, a, d, f);
        moved[key(q)] = f;
      }
      b.dropped = true;
      ++merged;
    }
  }
  for (RoadIn& r : roads) {
    if (r.kind != 5 || r.dropped) continue;
    for (auto& q : r.pts) {
      auto it = moved.find(key(q));
      if (it != moved.end()) q = it->second;
    }
  }
  roads.erase(std::remove_if(roads.begin(), roads.end(), [](const RoadIn& r) { return r.dropped; }), roads.end());
  if (merged) LOG_INFO("osm: %d railway tracks merged into double tracks", merged);
}

struct BuildingIn {
  std::string category;
  uint64_t id;
  int levels;
  std::vector<std::pair<double, double>> pts;
  bool polygon;  // pts is one closed ring (point-in-polygon works)
  bool terraced = false;  // wall to wall with a neighbour (or building=terrace): the game's city houses in a row
  int row = -1;           // the row of buildings wall to wall it belongs to (two or more), -1 none
};
// a tree to plant: its cell and what it grows in (0 wood, 1 scrub, 2 park, 3 a single tree or a row, 4 orchard)
struct TreeIn {
  int first = 0, second = 0;
  int kind = 3;
};
// a railway station (OSM railway=station / halt node), cells
struct StationIn {
  double x = 0, z = 0;
  std::string name;
};
struct ImportData {
  int W = 0, L = 0, flags = 0;
  std::vector<int16_t> heights;  // per corner, engine units (empty: terrain unchanged)
  std::vector<uint8_t> water;    // per cell
  std::vector<uint8_t> paving;   // per cell: 0 none, else the pavement paint type + 1 (2 industrial, 3 paving stones)
  std::vector<TreeIn> trees;
  std::vector<RoadIn> roads;
  std::vector<StationIn> stations;
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
           "motorway_link|trunk_link|primary_link|secondary_link|tertiary_link|pedestrian)$\"]" + b + ";way[\"natural\"=\"tree_row\"]" + b +
           ";way[\"place\"=\"square\"]" + b + ";way[\"amenity\"=\"parking\"]" + b + ";way[\"area:highway\"]" + b +
           ";way[\"railway\"~\"^(rail|light_rail)$\"]" + b + ";node[\"railway\"~\"^(station|halt)$\"]" + b + ";";
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
  struct TreeArea {
    Edges edges;
    double density;
    int kind;
  };
  std::vector<TreeArea> treeAreas;
  std::vector<std::pair<Edges, uint8_t>> pavedAreas;
  std::vector<std::vector<std::pair<double, double>>> pavedLines;
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
          if ((flags & kStreets) && (tags.str("railway") == "station" || tags.str("railway") == "halt")) {
            StationIn st;
            st.x = fr.cellX(lonToMx(el.num("lon"))), st.z = fr.cellZ(latToMy(el.num("lat")));
            st.name = tags.str("name");
            if (st.x >= 0 && st.z >= 0 && st.x < W && st.z < L) data->stations.push_back(st);
            continue;
          }
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
        // paved places (owner 2026-11-08): squares and pedestrian areas in paving stones, car parks in the industrial
        // pavement, pedestrian streets with a band of paving stones
        if (flags & kStreets) {
          bool square = tags.str("place") == "square" || tags.str("area:highway") == "pedestrian" ||
                        (hw == "pedestrian" && tags.str("area") == "yes");
          if (closed && square) pavedAreas.push_back({ringEdges, 3});
          else if (closed && tags.str("amenity") == "parking" && tags.str("parking") != "underground" &&
                   tags.str("parking") != "multi-storey")
            pavedAreas.push_back({ringEdges, 2});
          else if (hw == "pedestrian" && type == "way" && !closed)
            pavedLines.push_back(pts);
          if (square || tags.str("amenity") == "parking" || !tags.str("area:highway").empty()) continue;
        }
        // railways (owner 2026-11-13: tracks and the station were missing): main lines as the railway's double track
        if ((flags & kStreets) && type == "way" && (tags.str("railway") == "rail" || tags.str("railway") == "light_rail")) {
          std::string sv = tags.str("service");
          if (tags.str("tunnel") == "yes" || tags.num("layer", 0) < 0 || sv == "yard" || sv == "siding" || sv == "spur" ||
              sv == "crossover")
            continue;
          RoadIn r;
          r.kind = 5;
          r.bridge = tags.str("bridge") == "yes" || tags.str("bridge") == "viaduct";
          r.name = tags.str("name");
          r.pts = std::move(pts);
          data->roads.push_back(std::move(r));
          continue;
        }
        if (!hw.empty() && type == "way") {
          // tunnels: a short one (an underpass, up to 80 m) stays on the ground and what runs over it gets a bridge there
          // (owner 2026-11-13: a street under the railway did not reach its roundabout); longer ones are left out
          bool tunnel = tags.str("tunnel") == "yes" || tags.str("tunnel") == "building_passage" || tags.num("layer", 0) < 0;
          bool under = false;
          if (tunnel) {
            double len = 0;
            for (size_t i = 0; i + 1 < pts.size(); ++i)
              len += std::hypot(pts[i + 1].first - pts[i].first, pts[i + 1].second - pts[i].second) * cellM;
            if (len > 80.0) continue;
            under = true;
          }
          if (tags.str("area") == "yes") continue;
          RoadIn r;
          r.underpass = under;
          r.kind = hw == "pedestrian" ? 2 : 0;
          r.cls = (hw == "motorway" || hw == "trunk") ? 4 : hw == "primary" ? 3 : hw == "secondary" ? 2 : hw == "tertiary" ? 1 : 0;
          r.bridge = tags.str("bridge") == "yes" || tags.str("bridge") == "viaduct";
          std::string ow = tags.str("oneway");
          r.oneway = ow == "yes" || ow == "1" || ow == "-1" || hw == "motorway";
          if (ow == "-1") std::reverse(pts.begin(), pts.end());  // one-way against the way's direction
          std::string jn = tags.str("junction");
          r.ring = jn == "roundabout" || jn == "circular";
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
          bi.terraced = tags.str("building") == "terrace";
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
          if (closed) treeAreas.push_back({ringEdges, 1.0, 0});  // a forest: full of trees and bushes
        } else if (nat == "scrub") {
          if (closed) treeAreas.push_back({ringEdges, 1.0, 1});
        } else if (lu == "orchard") {
          if (closed) treeAreas.push_back({ringEdges, 1.0, 4});  // fruit trees in rows (every 4th cell each way)
        } else if (le == "park" || le == "garden") {
          if (closed) treeAreas.push_back({ringEdges, 0.08, 2});
        }
      }
  osm = Json();
  classifyRoads(data->roads, treeRows, cellM);
  mergeTracks(data->roads);
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
    for (auto& [edges, density, kind] : treeAreas) {
      ++salt;
      double d = density;
      int k = kind;
      fillEdges(edges, W, L, [&](int x, int z) {
        size_t i = size_t(z) * size_t(W) + size_t(x);
        if (taken[i] || water[i] || x < 1 || z < 1 || x >= W - 1 || z >= L - 1) return;
        if (k == 4 && ((x & 3) || (z & 3))) return;
        if (double(hash32((uint64_t(x) << 40) ^ (uint64_t(z) << 16) ^ salt) % 10000) >= d * 10000.0) return;
        taken[i] = 1;
        data->trees.push_back({x, z, k});
      });
    }
    if (std::getenv("REGS_DEBUG_OSM")) {
      int byKind[5] = {0, 0, 0, 0, 0}, areas[5] = {0, 0, 0, 0, 0};
      for (auto& t : data->trees) ++byKind[std::clamp(t.kind, 0, 4)];
      for (auto& ta : treeAreas) ++areas[std::clamp(ta.kind, 0, 4)];
      LOG_INFO("osm: trees by kind: wood %d (%d areas), scrub %d (%d), park %d (%d), single %d, orchard %d (%d)", byKind[0], areas[0],
               byKind[1], areas[1], byKind[2], areas[2], byKind[3], byKind[4], areas[4]);
    }
    if (data->trees.size() > 70000) {  // the game's maps hold about 90k objects in all (with undergrowth: ~100k)
      for (size_t i = data->trees.size() - 1; i > 0; --i) std::swap(data->trees[i], data->trees[hash32(i) % (i + 1)]);
      data->trees.resize(70000);
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
  // the paved places' cells
  if (!pavedAreas.empty() || !pavedLines.empty()) {
    data->paving.assign(size_t(W) * size_t(L), 0);
    for (auto& [edges, t] : pavedAreas)
      fillEdges(edges, W, L, [&](int x, int z) {
        size_t i = size_t(z) * size_t(W) + size_t(x);
        if (!water[i]) data->paving[i] = t;
      });
    const double half = 3.0 / cellM;  // a pedestrian street: 6 m of paving stones (4: buildings may stand on it)
    for (auto& line : pavedLines)
      for (size_t k = 0; k + 1 < line.size(); ++k) {
        auto a = line[k], b2 = line[k + 1];
        int x0 = int(std::floor(std::min(a.first, b2.first) - half)), x1 = int(std::ceil(std::max(a.first, b2.first) + half));
        int z0 = int(std::floor(std::min(a.second, b2.second) - half)), z1 = int(std::ceil(std::max(a.second, b2.second) + half));
        for (int z = std::max(0, z0); z <= std::min(L - 1, z1); ++z)
          for (int x = std::max(0, x0); x <= std::min(W - 1, x1); ++x) {
            size_t i = size_t(z) * size_t(W) + size_t(x);
            if (!water[i] && !data->paving[i] && segDist(x + 0.5, z + 0.5, a, b2) <= half) data->paving[i] = 4;
          }
      }
  }
  // buildings wall to wall (owner 2026-11-08: city houses in a row there): two corners of one on the other's outline
  {
    const double tol = 0.5 / cellM;
    const int B = 16;
    std::map<std::pair<int, int>, std::vector<size_t>> bucket;
    for (size_t i = 0; i < data->buildings.size(); ++i)
      for (auto& q : data->buildings[i].pts) {
        auto& v = bucket[{int(std::floor(q.first / B)), int(std::floor(q.second / B))}];
        if (v.empty() || v.back() != i) v.push_back(i);
      }
    int rows = 0;
    std::vector<size_t> parent(data->buildings.size());
    for (size_t i = 0; i < parent.size(); ++i) parent[i] = i;
    auto root = [&](size_t i) {
      while (parent[i] != i) i = parent[i] = parent[parent[i]];
      return i;
    };
    for (size_t i = 0; i < data->buildings.size(); ++i) {
      BuildingIn& b = data->buildings[i];
      if (!b.polygon || b.category == "industry" || b.category == "farm" || b.category == "government" || b.category == "railway")
        continue;
      std::set<size_t> near;
      for (auto& q : b.pts) {
        int bx = int(std::floor(q.first / B)), bz = int(std::floor(q.second / B));
        for (int dz = -1; dz <= 1; ++dz)
          for (int dx = -1; dx <= 1; ++dx) {
            auto it = bucket.find({bx + dx, bz + dz});
            if (it != bucket.end())
              for (size_t j : it->second)
                if (j != i) near.insert(j);
          }
      }
      for (size_t j : near) {
        const BuildingIn& o = data->buildings[j];
        if (!o.polygon) continue;
        int touching = 0;
        for (auto& q : b.pts) {
          for (size_t k = 0; k < o.pts.size(); ++k)
            if (segDist(q.first, q.second, o.pts[k], o.pts[(k + 1) % o.pts.size()]) < tol) {
              ++touching;
              break;
            }
          if (touching >= 2) break;
        }
        if (touching >= 2) {
          b.terraced = true;
          parent[root(i)] = root(j);
        }
      }
      rows += b.terraced;
    }
    std::map<size_t, int> size, id;
    for (size_t i = 0; i < data->buildings.size(); ++i)
      if (data->buildings[i].terraced) ++size[root(i)];
    for (size_t i = 0; i < data->buildings.size(); ++i) {
      size_t r = root(i);
      if (!data->buildings[i].terraced || size[r] < 2) continue;
      auto it = id.find(r);
      if (it == id.end()) it = id.emplace(r, int(id.size())).first;
      data->buildings[i].row = it->second;
    }
    LOG_INFO("osm: %d buildings wall to wall, %zu rows", rows, id.size());
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
  bool gridStreets = false;
  std::vector<char> inRow;  // buildings a row of city houses took
  int stationsPlaced = 0;  // REGS_OSM_GRID: the streets on the block grid (before the curve tools)
  int curvesMade = 0;
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

// ---- the streets as curves (owner's request 2026-11-08: the port's curve tools, docs/building-tools.md) ----
// Every OSM way becomes a chain of curves (one cubic Bézier per leg, Catmull-Rom tangents) through its simplified
// points; ways sharing an OSM node share the exact point, so the curves meet there in a junction (three and more arms,
// or a sharp bend) or join smoothly (two arms running on). The limits of the curve tool hold: legs shorter than the
// junctions at their ends need are merged away (a point dropped, or two junctions made one), arms closer than 30° are
// spread, bends tighter than the kind's minimum radius become junctions. Ways leaving the map end on two grid blocks at
// the edge (the game's edge piece, then a straight piece the curve starts on: traffic from outside). Water and the
// map's border cut ways (bridges later); bridges crossing other streets without a shared node are left out; footpaths
// keep away from roads (curve junctions take footpaths only among themselves).
namespace streets {

constexpr double kPiD = 3.14159265358979323846;

struct Node {
  double x = 0, z = 0;  // world units
  float y = 0;
  bool foot = false;
  int exit = -1;        // the map-edge stub it ends on
  bool corner = false;  // a sharp bend: its two roads meet as a junction
  bool fixed = false;   // a roundabout's point: never moved, merged or dropped
  bool track = false;   // a track's point: tracks have no junction pieces (they only join smoothly)
  bool alive = true;
};
struct Seg {
  int a = 0, b = 0;   // nodes; a → b is the way one-way streets run
  int kind = 0;       // CurveRoad::kind
  uint8_t flags = 0;  // CurveRoad::ends bits 0x20 / 0x40 / 0x80
  bool alive = true;
  bool fixed = false;  // a roundabout's arc: its own tangents and handles below, never tidied away
  bool bridge = false;     // a bridge curve (CurveRoad::bridge): an OSM bridge, or a leg over water
  bool osmBridge = false;  // tagged bridge=yes (it may cross streets: its deck clears them)
  double d0x = 0, d0z = 0, d1x = 0, d1z = 0, h = 0;
};
struct Exit {
  int side = 0;      // 0 west (−x), 1 east, 2 north (−z), 3 south
  int bx = 0, bz = 0, ex = 0, ez = 0;  // the inner block, the edge block
  int type = 2;      // the blocks' road item type: 2 small, 5 large
  double px = 0, pz = 0;  // the expected end point (world)
  double dx = 0, dz = 0;  // into the map
  int node = -1;
};
struct Plan {
  std::vector<Node> nodes;
  std::vector<Seg> segs;
  std::vector<Exit> exits;
  int dropped = 0, merged = 0, corners = 0;
};

float halfWidth(int kind) { return curveProfile(kind).halfWidth; }
double minLen(int kind) { return kind == 3 ? 3.0 : 6.0; }

using Pt = std::pair<double, double>;
struct Chain {
  std::vector<Pt> pts;  // cells
  int kind = 0;
  uint8_t flags = 0;
  bool bridge = false;
  bool underpass = false;
  int exit0 = -1, exit1 = -1;  // the map side the first / last point lies on (clipped there), -1 none
};

// a polyline's runs where bad() is false, tested every `step` cells; the original points stay (shared nodes keep their
// exact place), the cuts back off `back` cells from the bad part
template <class F>
std::vector<Chain> splitWhere(const Chain& c, F bad, double step, double back, double bridge = 0, bool keep0 = false,
                              bool keep1 = false) {
  struct S {
    Pt p;
    bool orig, bad;
  };
  std::vector<S> ss;
  for (size_t i = 0; i + 1 < c.pts.size(); ++i) {
    Pt p = c.pts[i], q = c.pts[i + 1];
    int n = std::max(1, int(std::ceil(std::hypot(q.first - p.first, q.second - p.second) / step)));
    for (int k = 0; k < n; ++k) {
      double t = double(k) / n;
      Pt s{p.first + (q.first - p.first) * t, p.second + (q.second - p.second) * t};
      ss.push_back({s, k == 0, false});
    }
  }
  ss.push_back({c.pts.back(), true, false});
  for (S& x : ss) x.bad = bad(x.p.first, x.p.second);
  // bad stretches no longer than bridge cells between good ones stay (a stream the street crosses on a causeway)
  if (bridge > 0)
    for (size_t i = 1; i < ss.size(); ++i) {
      if (!ss[i].bad || ss[i - 1].bad) continue;
      size_t j = i;
      while (j < ss.size() && ss[j].bad) ++j;
      if (j < ss.size() && std::hypot(ss[j].p.first - ss[i - 1].p.first, ss[j].p.second - ss[i - 1].p.second) <= bridge)
        for (size_t k = i; k < j; ++k) ss[k].bad = false;
      i = j;
    }
  // a bad stretch at an end that joins another street there (a bridge's end on the bank): kept, up to bridge cells
  if (bridge > 0 && keep0 && ss.front().bad) {
    size_t j = 0;
    while (j < ss.size() && ss[j].bad) ++j;
    if (j < ss.size() && std::hypot(ss[j].p.first - ss[0].p.first, ss[j].p.second - ss[0].p.second) <= bridge)
      for (size_t k = 0; k < j; ++k) ss[k].bad = false;
  }
  if (bridge > 0 && keep1 && ss.back().bad) {
    size_t j = ss.size() - 1;
    while (j > 0 && ss[j].bad) --j;
    if (!ss[j].bad && std::hypot(ss.back().p.first - ss[j].p.first, ss.back().p.second - ss[j].p.second) <= bridge)
      for (size_t k = j + 1; k < ss.size(); ++k) ss[k].bad = false;
  }
  std::vector<Chain> out;
  auto dist = [](Pt a, Pt b) { return std::hypot(a.first - b.first, a.second - b.second); };
  for (size_t i = 0; i < ss.size();) {
    if (ss[i].bad) {
      ++i;
      continue;
    }
    size_t j = i;
    while (j + 1 < ss.size() && !ss[j + 1].bad) ++j;
    size_t from = i, to = j;
    if (i > 0)
      while (from < to && dist(ss[from].p, ss[i - 1].p) < back) ++from;
    if (j + 1 < ss.size())
      while (to > from && dist(ss[to].p, ss[j + 1].p) < back) --to;
    if (to > from) {
      Chain part = c;
      part.pts.clear();
      part.exit0 = i == 0 ? c.exit0 : -1;
      part.exit1 = j + 1 == ss.size() ? c.exit1 : -1;
      part.pts.push_back(ss[from].p);
      for (size_t k = from + 1; k < to; ++k)
        if (ss[k].orig) part.pts.push_back(ss[k].p);
      part.pts.push_back(ss[to].p);
      out.push_back(std::move(part));
    }
    i = j + 1;
  }
  return out;
}

// the parts of a chain inside [lo, hi] (cells), the cut ends flagged with their side
std::vector<Chain> clipChain(const Chain& c, double x0, double z0, double x1, double z1) {
  std::vector<Chain> out;
  Chain cur = c;
  cur.pts.clear();
  cur.exit0 = cur.exit1 = -1;
  for (size_t i = 0; i + 1 < c.pts.size(); ++i) {
    Pt p = c.pts[i], q = c.pts[i + 1];
    double dx = q.first - p.first, dz = q.second - p.second, t0 = 0, t1 = 1;
    int s0 = -1, s1 = -1;
    auto clip = [&](double num, double den, int side) {  // num + den·t >= 0
      if (std::fabs(den) < 1e-12) return num >= 0;
      double t = -num / den;
      if (den > 0) {
        if (t > t0) t0 = t, s0 = side;
      } else if (t < t1) t1 = t, s1 = side;
      return true;
    };
    bool ok = clip(p.first - x0, dx, 0) && clip(x1 - p.first, -dx, 1) && clip(p.second - z0, dz, 2) && clip(z1 - p.second, -dz, 3);
    if (!ok || t0 > t1) {
      if (cur.pts.size() >= 2) out.push_back(cur);
      cur.pts.clear(), cur.exit0 = cur.exit1 = -1;
      continue;
    }
    Pt a{p.first + dx * t0, p.second + dz * t0}, b{p.first + dx * t1, p.second + dz * t1};
    if (cur.pts.empty()) {
      cur.pts.push_back(t0 > 0 ? a : p);
      cur.exit0 = t0 > 0 ? s0 : -1;
    }
    cur.pts.push_back(t1 < 1 ? b : q);
    if (t1 < 1) {
      cur.exit1 = s1;
      if (cur.pts.size() >= 2) out.push_back(cur);
      cur.pts.clear(), cur.exit0 = cur.exit1 = -1;
    }
  }
  if (cur.pts.size() >= 2) out.push_back(cur);
  return out;
}

bool segCross(Pt a, Pt b, Pt c, Pt d) {  // proper crossing (not at the ends)
  auto cr = [](Pt o, Pt p, Pt q) { return (p.first - o.first) * (q.second - o.second) - (p.second - o.second) * (q.first - o.first); };
  double d1 = cr(c, d, a), d2 = cr(c, d, b), d3 = cr(a, b, c), d4 = cr(a, b, d);
  return ((d1 > 1e-9 && d2 < -1e-9) || (d1 < -1e-9 && d2 > 1e-9)) && ((d3 > 1e-9 && d4 < -1e-9) || (d3 < -1e-9 && d4 > 1e-9));
}

Plan plan(const std::vector<RoadIn>& roads, int W, int L, float cs, const std::vector<uint8_t>& water) {
  Plan P;
  // the points several streets share (an end there joins another street: kept even in water)
  std::map<std::pair<long long, long long>, int> sharedPts;
  auto pkey = [](Pt q) { return std::pair<long long, long long>{std::llround(q.first * 64), std::llround(q.second * 64)}; };
  for (const RoadIn& r : roads)
    for (Pt q : r.pts) ++sharedPts[pkey(q)];
  // 1. chains in cells: the curve kinds, one-way flags, clipped to the map less the edge stubs, cut at water
  std::vector<Chain> chains;
  for (const RoadIn& r : roads) {
    if (r.pts.size() < 2) continue;
    Chain c;
    c.pts = r.pts;
    c.kind = r.kind == 5 ? 6 : r.kind == 2 ? 3 : r.kind == 3 ? 2 : (r.kind == 1 || r.kind == 4) ? 1 : 0;
    c.bridge = r.bridge;
    c.underpass = r.underpass;
    if (r.ring && r.kind != 5) c.kind = 0;  // roundabouts: one-way small roads (large ones are not supported yet)
    if (c.kind == 0 && (r.oneway || r.ring)) {
      bool single = r.lanes == 1 || (r.lanes == 0 && (r.ring || r.cls == 0));
      c.flags = uint8_t(0x20 | (single ? 0x40 : 0) | (r.ring ? 0x80 : 0));
    }
    const double m = c.kind == 3 ? 2.0 : c.kind == 0 ? 8.0 : 14.0;
    for (Chain& part : clipChain(c, m, m, W - m, L - m)) {
      auto wet = [&](double x, double z) {
        int cx = int(std::floor(x)), cz = int(std::floor(z));
        if (cx < 0 || cz < 0 || cx >= W || cz >= L) return true;
        return water[size_t(cz) * size_t(W) + size_t(cx)] != 0;
      };
      // streams up to 60 m wide: the street crosses on a causeway (bridges are not built yet)
      if (part.bridge) chains.push_back(part);  // a bridge crosses whatever water it spans (on a causeway for now)
      else
        for (Chain& dry : splitWhere(part, wet, 0.5, 1.0, 60.0 / cs, sharedPts[pkey(part.pts.front())] > 1,
                                      sharedPts[pkey(part.pts.back())] > 1))
          chains.push_back(std::move(dry));
    }
  }
  // 2. bridges crossing other streets without a shared node go (the legs that cross); footpaths keep 1.5 units off roads
  {
    const int B = 8;  // a bucket of the road legs per 8 × 8 cells
    const int bw = W / B + 1, bl = L / B + 1;
    std::vector<std::vector<std::pair<int, int>>> bucket(size_t(bw) * size_t(bl));
    auto addLeg = [&](int ci, int li) {
      const Chain& c = chains[size_t(ci)];
      Pt p = c.pts[size_t(li)], q = c.pts[size_t(li) + 1];
      int x0 = std::clamp(int(std::min(p.first, q.first)) / B, 0, bw - 1), x1 = std::clamp(int(std::max(p.first, q.first)) / B, 0, bw - 1);
      int z0 = std::clamp(int(std::min(p.second, q.second)) / B, 0, bl - 1), z1 = std::clamp(int(std::max(p.second, q.second)) / B, 0, bl - 1);
      for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) bucket[size_t(z) * size_t(bw) + size_t(x)].push_back({ci, li});
    };
    for (size_t ci = 0; ci < chains.size(); ++ci)
      if (chains[ci].kind != 3)
        for (size_t li = 0; li + 1 < chains[ci].pts.size(); ++li) addLeg(int(ci), int(li));
    auto near = [&](double x, double z, double r, auto f) {
      int bx0 = std::clamp(int((x - r) / B), 0, bw - 1), bx1 = std::clamp(int((x + r) / B), 0, bw - 1);
      int bz0 = std::clamp(int((z - r) / B), 0, bl - 1), bz1 = std::clamp(int((z + r) / B), 0, bl - 1);
      for (int bz = bz0; bz <= bz1; ++bz)
        for (int bx = bx0; bx <= bx1; ++bx)
          for (auto [ci, li] : bucket[size_t(bz) * size_t(bw) + size_t(bx)])
            if (f(ci, li)) return true;
      return false;
    };
    std::vector<Chain> kept;
    for (size_t ci = 0; ci < chains.size(); ++ci) {
      const Chain& c = chains[ci];
      if (c.kind == 3) {
        auto byRoad = [&](double x, double z) {
          return near(x, z, 6.0, [&](int cj, int lj) {
            const Chain& o = chains[size_t(cj)];
            double lim = (halfWidth(o.kind) + 1.5) / cs;
            return segDist(x, z, o.pts[size_t(lj)], o.pts[size_t(lj) + 1]) < lim;
          });
        };
        for (Chain& part : splitWhere(c, byRoad, 0.5, 0.0)) kept.push_back(std::move(part));
        continue;
      }
      if (c.bridge) {  // bridges stay whole (owner 2026-11-13: streets over water must stay joined)
        kept.push_back(c);
        continue;
      }
      // a street a bridge crosses without a shared node runs under it (the bridge's deck clears it)
      if (true) {
        kept.push_back(c);
        continue;
      }
      std::vector<std::pair<int, int>> over;
      for (size_t li = 0; li + 1 < c.pts.size(); ++li) {
        Pt p = c.pts[li], q = c.pts[li + 1];
        near((p.first + q.first) / 2, (p.second + q.second) / 2, std::hypot(q.first - p.first, q.second - p.second) / 2 + 1,
             [&](int cj, int lj) {
               const Chain& o = chains[size_t(cj)];
               if (size_t(cj) != ci && o.bridge && segCross(p, q, o.pts[size_t(lj)], o.pts[size_t(lj) + 1])) over.push_back({cj, lj});
               return false;
             });
      }
      if (over.empty()) {
        kept.push_back(c);
        continue;
      }
      auto under = [&](double x, double z) {
        for (auto [cj, lj] : over) {
          const Chain& o = chains[size_t(cj)];
          if (segDist(x, z, o.pts[size_t(lj)], o.pts[size_t(lj) + 1]) < (halfWidth(o.kind) + halfWidth(c.kind) + 2.0) / cs) return true;
        }
        return false;
      };
      for (Chain& part : splitWhere(c, under, 0.5, 0.0)) kept.push_back(std::move(part));
    }
    chains.swap(kept);
    // bridges just where they are needed: a leg's stretches over water (a cell of dry bank each side) and, for tracks,
    // over an underpass become their own short bridge chains (owner 2026-11-13: not a whole simplified leg as a viaduct)
    std::vector<Chain> out;
    auto wetAt = [&](Pt q) {
      int cx = int(std::floor(q.first)), cz = int(std::floor(q.second));
      return cx >= 0 && cz >= 0 && cx < W && cz < L && water[size_t(cz) * size_t(W) + size_t(cx)] != 0;
    };
    for (const Chain& c : chains) {
      if (c.kind == 3 || c.bridge) {
        out.push_back(c);
        continue;
      }
      Chain cur = c;
      cur.pts.assign(1, c.pts.front());
      for (size_t li = 0; li + 1 < c.pts.size(); ++li) {
        Pt p = c.pts[li], q = c.pts[li + 1];
        double len = std::max(1e-9, std::hypot(q.first - p.first, q.second - p.second));
        std::vector<std::pair<double, double>> iv;  // stretches (0..1) to bridge
        if (curveIsTrack(c.kind))
          for (const Chain& u : chains) {
            if (!u.underpass) continue;
            for (size_t k = 0; k + 1 < u.pts.size(); ++k) {
              Pt ua = u.pts[k], ub = u.pts[k + 1];
              if (!segCross(p, q, ua, ub)) continue;
              double dx = q.first - p.first, dz = q.second - p.second, ex = ub.first - ua.first, ez = ub.second - ua.second;
              double den = dx * ez - dz * ex;
              if (std::fabs(den) < 1e-12) continue;
              double t = ((ua.first - p.first) * ez - (ua.second - p.second) * ex) / den;
              double half = (halfWidth(0) + 4.0) / cs / len;
              iv.push_back({t - half, t + half});
            }
          }
        {
          int n = std::max(2, int(std::ceil(len / 0.25)));
          int runStart = -1;
          for (int k = 0; k <= n; ++k) {
            double t = double(k) / n;
            bool w = wetAt({p.first + (q.first - p.first) * t, p.second + (q.second - p.second) * t});
            if (w && runStart < 0) runStart = k;
            if ((!w || k == n) && runStart >= 0) {
              double m = 1.5 / len;  // a cell and a half of bank each side
              iv.push_back({double(runStart) / n - m, (w ? t : double(k - 1) / n) + m});
              runStart = -1;
            }
          }
        }
        std::sort(iv.begin(), iv.end());
        std::vector<std::pair<double, double>> merged;
        for (auto [t0, t1] : iv) {
          t0 = std::max(0.0, t0), t1 = std::min(1.0, t1);
          if (t1 <= t0) continue;
          if (!merged.empty() && t0 <= merged.back().second + 1.0 / len) merged.back().second = std::max(merged.back().second, t1);
          else merged.push_back({t0, t1});
        }
        double from = 0;
        for (auto [t0, t1] : merged) {
          // a stretch reaching a leg's end takes the end point itself (a shared node stays shared)
          if (t0 < 0.5 / len) t0 = 0;
          if (t1 > 1 - 0.5 / len) t1 = 1;
          t0 = std::max(from, t0);
          if (t1 <= t0) continue;
          Pt b0 = t0 <= 0 ? p : Pt{p.first + (q.first - p.first) * t0, p.second + (q.second - p.second) * t0};
          Pt b1 = t1 >= 1 ? q : Pt{p.first + (q.first - p.first) * t1, p.second + (q.second - p.second) * t1};
          if (t0 > from + 1e-9) cur.pts.push_back(b0);
          if (cur.pts.size() >= 2) {
            cur.exit1 = -1;
            out.push_back(cur);
          }
          Chain br = c;
          br.pts = {b0, b1};
          br.bridge = true;
          br.exit0 = t0 <= 0 && li == 0 ? c.exit0 : -1;
          br.exit1 = -1;
          out.push_back(br);
          cur.pts.assign(1, b1);
          cur.exit0 = -1;
          from = t1;
        }
        if (from < 1.0 - 1e-9) cur.pts.push_back(q);
      }
      if (cur.pts.size() >= 2) out.push_back(cur);
    }
    chains.swap(out);
  }
  // 3. nodes: shared OSM points are one node (footpaths their own), simplified chains, legs
  // the node classes: roads, footpaths, each track kind (they meet only their own)
  auto key = [](Pt q, int cls) {
    return std::tuple<long long, long long, int>{std::llround(q.first * 64), std::llround(q.second * 64), cls};
  };
  std::map<std::tuple<long long, long long, int>, int> nodeOf;
  std::map<int, int> chainsAt;  // node → how many chains pass it
  std::vector<std::vector<int>> ids(chains.size());
  for (size_t ci = 0; ci < chains.size(); ++ci) {
    std::set<int> seen;
    for (Pt q : chains[ci].pts) {
      auto k = key(q, curveJoinClass(chains[ci].kind));
      auto it = nodeOf.find(k);
      int id;
      if (it == nodeOf.end()) {
        id = int(P.nodes.size());
        Node n;
        n.x = (q.first - W * 0.5) * cs, n.z = (q.second - L * 0.5) * cs, n.foot = chains[ci].kind == 3;
        n.track = curveIsTrack(chains[ci].kind);
        P.nodes.push_back(n);
        nodeOf[k] = id;
      } else id = it->second;
      ids[ci].push_back(id);
      if (seen.insert(id).second) ++chainsAt[id];
      else chainsAt[id] += 2;  // a loop closing on itself
    }
  }
  // the map-edge stubs
  auto makeExit = [&](int nodeId, int side, int kind) {
    if (kind == 3 || kind == 2 || kind >= 4) return;  // footpaths, avenues and tracks end free at the edge
    Node& n = P.nodes[size_t(nodeId)];
    double cx = n.x / cs + W * 0.5, cz = n.z / cs + L * 0.5;
    Exit e;
    e.side = side;
    const bool large = kind == 1;
    e.type = large ? 5 : 2;
    const int depth = large ? 8 : 4, reach = large ? 14 : 8;  // the inner block's offset, the end's distance from the edge
    bool alongX = side >= 2;  // the stub runs along z: the edge is north / south, the free coordinate x
    double along = alongX ? cx : cz;
    int edgeAt = side == 0 || side == 2 ? 0 : (alongX ? L : W) - 4;
    int off = 0;
    if (large) off = ((-edgeAt) % 8 + 8) % 8;  // (x + z) & 4 == 0 on the block lattice
    int step = large ? 8 : 4;
    int a = int(std::lround((along - 2 - off) / step)) * step + off;
    int lim = alongX ? W : L;
    if (a < 16 || a > lim - 20) return;  // near a corner
    int inner = side == 0 || side == 2 ? edgeAt + depth : edgeAt - depth;
    if (alongX) e.bx = a, e.bz = inner, e.ex = a, e.ez = edgeAt;
    else e.bx = inner, e.bz = a, e.ex = edgeAt, e.ez = a;
    for (const Exit& o : P.exits)  // stubs keep a block apart
      if (o.side == side && std::abs((alongX ? o.bx : o.bz) - a) < (large || o.type == 5 ? 12 : 8)) return;
    double endCell = side == 0 || side == 2 ? reach : (alongX ? L : W) - reach;
    double ax = alongX ? a + 2.0 : endCell, az = alongX ? endCell : a + 2.0;
    e.px = (ax - W * 0.5) * cs, e.pz = (az - L * 0.5) * cs;
    e.dx = side == 0 ? 1 : side == 1 ? -1 : 0;
    e.dz = side == 2 ? 1 : side == 3 ? -1 : 0;
    e.node = nodeId;
    n.x = e.px, n.z = e.pz;
    n.exit = int(P.exits.size());
    P.exits.push_back(e);
  };
  for (size_t ci = 0; ci < chains.size(); ++ci) {
    const Chain& c = chains[ci];
    std::vector<int>& v = ids[ci];
    std::vector<char> keep(v.size(), 0);
    keep.front() = keep.back() = 1;
    for (size_t i = 0; i < v.size(); ++i)
      if (chainsAt[v[i]] > 1) keep[i] = 1;
    // Douglas-Peucker between the kept points (1.2 units)
    std::vector<std::pair<size_t, size_t>> st;
    size_t prev = 0;
    for (size_t i = 1; i < v.size(); ++i)
      if (keep[i]) st.push_back({prev, i}), prev = i;
    while (!st.empty()) {
      auto [ia, ib] = st.back();
      st.pop_back();
      const Node &A = P.nodes[size_t(v[ia])], &Bn = P.nodes[size_t(v[ib])];
      double dx = Bn.x - A.x, dz = Bn.z - A.z, len = std::hypot(dx, dz);
      size_t best = 0;
      double bd = 0;
      for (size_t i = ia + 1; i < ib; ++i) {
        const Node& Q = P.nodes[size_t(v[i])];
        double d = len > 1e-9 ? std::fabs((Q.x - A.x) * dz - (Q.z - A.z) * dx) / len : std::hypot(Q.x - A.x, Q.z - A.z);
        if (d > bd) bd = d, best = i;
      }
      if (bd > 1.2) {
        keep[best] = 1;
        st.push_back({ia, best});
        st.push_back({best, ib});
      }
    }
    if (c.exit0 >= 0 && P.nodes[size_t(v.front())].exit < 0 && chainsAt[v.front()] == 1) makeExit(v.front(), c.exit0, c.kind);
    if (c.exit1 >= 0 && P.nodes[size_t(v.back())].exit < 0 && chainsAt[v.back()] == 1) makeExit(v.back(), c.exit1, c.kind);
    int last = -1;
    for (size_t i = 0; i < v.size(); ++i) {
      if (!keep[i]) continue;
      if (last >= 0 && last != v[i]) {
        Seg s;
        s.a = last, s.b = v[i], s.kind = c.kind, s.flags = c.flags;
        s.bridge = s.osmBridge = c.bridge;
        P.segs.push_back(s);
      }
      last = v[i];
    }
  }
  // legs over water are bridges (owner 2026-11-13: streets over the Saalbach; the curve tool's bridges since main d40011f)
  for (Seg& sg : P.segs) {
    if (sg.bridge || sg.kind == 3) continue;
    const Node &A = P.nodes[size_t(sg.a)], &B = P.nodes[size_t(sg.b)];
    double len = std::hypot(B.x - A.x, B.z - A.z) / cs;
    int n = std::max(2, int(std::ceil(len / 0.5)));
    for (int k = 1; k < n && !sg.bridge; ++k) {
      double t = double(k) / n;
      int cx = int(std::floor((A.x + (B.x - A.x) * t) / cs + W * 0.5)), cz = int(std::floor((A.z + (B.z - A.z) * t) / cs + L * 0.5));
      if (cx >= 0 && cz >= 0 && cx < W && cz < L && water[size_t(cz) * size_t(W) + size_t(cx)]) sg.bridge = true;
    }
  }
  return P;
}

// ---- the graph's geometry ----
struct Geo {
  std::vector<std::vector<int>> arms;  // per node its live segments
};
Geo arms(const Plan& P) {
  Geo g;
  g.arms.assign(P.nodes.size(), {});
  for (size_t i = 0; i < P.segs.size(); ++i)
    if (P.segs[i].alive) g.arms[size_t(P.segs[i].a)].push_back(int(i)), g.arms[size_t(P.segs[i].b)].push_back(int(i));
  return g;
}
int otherEnd(const Seg& s, int n) { return s.a == n ? s.b : s.a; }
void chordDir(const Plan& P, int from, int to, double& dx, double& dz) {
  dx = P.nodes[size_t(to)].x - P.nodes[size_t(from)].x, dz = P.nodes[size_t(to)].z - P.nodes[size_t(from)].z;
  double l = std::hypot(dx, dz);
  if (l > 1e-9) dx /= l, dz /= l;
}
// the direction a leg leaves node n: a roundabout arc's tangent, else the chord
void armDir(const Plan& P, int si, int n, double& dx, double& dz) {
  const Seg& s = P.segs[size_t(si)];
  if (s.fixed) {
    if (s.a == n) dx = s.d0x, dz = s.d0z;
    else dx = -s.d1x, dz = -s.d1z;
    return;
  }
  chordDir(P, n, otherEnd(s, n), dx, dz);
}
// a node where two roads run on (no junction): two arms, no sharp bend, not an edge stub
bool smooth(const Plan& P, const Geo& G, int n) {
  const auto& a = G.arms[size_t(n)];
  if (a.size() != 2 || P.nodes[size_t(n)].corner || P.nodes[size_t(n)].exit >= 0 || P.nodes[size_t(n)].fixed) return false;
  // a change of road kind: a junction piece joins the two widths (a bent join between kinds does not fit)
  if (P.segs[size_t(a[0])].kind != P.segs[size_t(a[1])].kind) return false;
  double ax, az, bx, bz;
  chordDir(P, n, otherEnd(P.segs[size_t(a[0])], n), ax, az);
  chordDir(P, n, otherEnd(P.segs[size_t(a[1])], n), bx, bz);
  return ax * bx + az * bz < -0.5;  // a bend of at most 60°
}
bool junction(const Plan& P, const Geo& G, int n) {
  return G.arms[size_t(n)].size() >= 3 || (G.arms[size_t(n)].size() == 2 && !smooth(P, G, n));
}
// the arms' directions away from a junction (spread to 32° at least) and its radius, as findCurveJunctions finds it
struct JunctionShape {
  std::map<int, std::pair<double, double>> dir;  // segment → direction
  double radius = 0;
};
JunctionShape junctionShape(const Plan& P, const Geo& G, int n) {
  JunctionShape J;
  struct A {
    int seg;
    double ang;
    float hw;
  };
  std::vector<A> as;
  for (int s : G.arms[size_t(n)]) {
    double dx, dz;
    armDir(P, s, n, dx, dz);
    as.push_back({s, std::atan2(dz, dx), halfWidth(P.segs[size_t(s)].kind)});
  }
  std::sort(as.begin(), as.end(), [](const A& x, const A& y) { return x.ang < y.ang; });
  const double minGap = 32.0 * kPiD / 180.0;
  for (int it = 0; it < 6 && as.size() >= 2; ++it) {
    bool moved = false;
    for (size_t k = 0; k < as.size(); ++k) {
      A &p = as[k], &q = as[(k + 1) % as.size()];
      double gap = k + 1 < as.size() ? q.ang - p.ang : q.ang + 2 * kPiD - p.ang;
      if (gap >= minGap) continue;
      double push = (minGap - gap) * 0.5 + 0.005;
      p.ang -= push, q.ang += push;
      moved = true;
    }
    if (!moved) break;
    for (A& a : as) a.ang = std::remainder(a.ang, 2 * kPiD);
    std::sort(as.begin(), as.end(), [](const A& x, const A& y) { return x.ang < y.ang; });
  }
  double R = 0;
  for (const A& a : as) R = std::max(R, double(a.hw));
  for (size_t k = 0; k < as.size(); ++k) {
    const A &p = as[k], &q = as[(k + 1) % as.size()];
    double gap = k + 1 < as.size() ? q.ang - p.ang : q.ang + 2 * kPiD - p.ang;
    if (gap < kPiD - 0.05 && gap > 0.01) R = std::max(R, double(std::max(p.hw, q.hw)) / std::tan(gap * 0.5));
  }
  bool foot = P.nodes[size_t(n)].foot;
  J.radius = std::min(R + (foot ? 0.5 : 1.0), 40.0);
  for (const A& a : as) J.dir[a.seg] = {std::cos(a.ang), std::sin(a.ang)};
  return J;
}

// OSM roundabouts become the game's kind of roundabout (owner 2026-11-13: a street did not reach its roundabout; on a
// scaled-down map the real ring is tighter than a curve may bend): a circle around the ring's centre, its radius the
// real one but at least what its arms need (10-40 units), one-way counter-clockwise (right-hand traffic, as the curve
// tool's roundabouts), every road that met the ring joined to it at its own angle, arcs of at most 90°
void rebuildRings(Plan& P) {
  std::vector<int> parent(P.nodes.size());
  for (size_t i = 0; i < parent.size(); ++i) parent[i] = int(i);
  auto root = [&](int i) {
    while (parent[size_t(i)] != i) i = parent[size_t(i)] = parent[size_t(parent[size_t(i)])];
    return i;
  };
  bool any = false;
  for (const Seg& sg : P.segs)
    if (sg.alive && (sg.flags & 0x80)) parent[size_t(root(sg.a))] = root(sg.b), any = true;
  if (!any) return;
  std::map<int, std::vector<int>> rings;  // root → its ring nodes
  std::vector<char> onRing(P.nodes.size(), 0);
  for (const Seg& sg : P.segs)
    if (sg.alive && (sg.flags & 0x80)) onRing[size_t(sg.a)] = onRing[size_t(sg.b)] = 1;
  for (size_t n = 0; n < P.nodes.size(); ++n)
    if (onRing[n]) rings[root(int(n))].push_back(int(n));
  for (auto& [r0, members] : rings) {
    if (members.size() < 3) continue;
    double cx = 0, cz = 0;
    for (int n : members) cx += P.nodes[size_t(n)].x, cz += P.nodes[size_t(n)].z;
    cx /= double(members.size()), cz /= double(members.size());
    double rr = 0;
    for (int n : members) rr += std::hypot(P.nodes[size_t(n)].x - cx, P.nodes[size_t(n)].z - cz);
    rr /= double(members.size());
    bool single = false;
    std::set<int> mem(members.begin(), members.end());
    struct Arm {
      int seg, node;
      double ang;
    };
    std::vector<Arm> arms;
    for (size_t si = 0; si < P.segs.size(); ++si) {
      Seg& sg = P.segs[si];
      if (!sg.alive) continue;
      if (sg.flags & 0x80) {
        if (mem.count(sg.a)) single = single || (sg.flags & 0x40), sg.alive = false;
        continue;
      }
      for (int n : {sg.a, sg.b})
        if (mem.count(n)) {
          const Node& N = P.nodes[size_t(n)];
          arms.push_back({int(si), n, std::atan2(N.z - cz, N.x - cx)});
        }
    }
    const double R = std::clamp(std::max(rr, 4.5 * double(arms.size()) + 4.0), 10.0, 40.0);
    for (int n : members) P.nodes[size_t(n)].alive = false;
    // the points on the circle: one per arm (arms closer than 20° share one), more where arcs pass 90°
    std::sort(arms.begin(), arms.end(), [](const Arm& x, const Arm& y) { return x.ang < y.ang; });
    std::vector<std::pair<double, int>> pts;  // angle, node
    const bool foot = false;
    auto newNode = [&](double ang) {
      Node nd;
      nd.x = cx + R * std::cos(ang), nd.z = cz + R * std::sin(ang), nd.foot = foot, nd.fixed = true;
      P.nodes.push_back(nd);
      return int(P.nodes.size() - 1);
    };
    for (const Arm& a : arms) {
      int node = -1;
      for (auto& [ang, nd] : pts)
        if (std::fabs(std::remainder(a.ang - ang, 2 * kPiD)) < 20.0 * kPiD / 180.0) node = nd;
      if (node < 0) node = newNode(a.ang), pts.push_back({a.ang, node});
      Seg& sg = P.segs[size_t(a.seg)];
      if (sg.a == a.node) sg.a = node;
      if (sg.b == a.node) sg.b = node;
    }
    if (pts.empty()) pts.push_back({0.0, newNode(0.0)});
    std::sort(pts.begin(), pts.end());
    std::vector<std::pair<double, int>> all;
    for (size_t k = 0; k < pts.size(); ++k) {
      all.push_back(pts[k]);
      double a0 = pts[k].first, a1 = k + 1 < pts.size() ? pts[k + 1].first : pts[0].first + 2 * kPiD;
      int parts = std::max(1, int(std::ceil((a1 - a0) / (kPiD / 2) - 1e-6)));
      for (int q = 1; q < parts; ++q) {
        double ang = a0 + (a1 - a0) * q / parts;
        all.push_back({ang, newNode(ang)});
      }
    }
    // the arcs, from each point to the one before it (counter-clockwise seen from above: the angle falls)
    for (size_t k = 0; k < all.size(); ++k) {
      auto [aHi, nHi] = all[(k + 1) % all.size()];
      auto [aLo, nLo] = all[k];
      double span = aHi - aLo;
      if (span <= 0) span += 2 * kPiD;
      Seg sg;
      sg.a = nHi, sg.b = nLo, sg.kind = 0, sg.fixed = true;
      sg.flags = uint8_t(0x20 | 0x80 | (single ? 0x40 : 0));
      sg.d0x = std::sin(aHi), sg.d0z = -std::cos(aHi);
      sg.d1x = std::sin(aLo), sg.d1z = -std::cos(aLo);
      sg.h = R * 4.0 / 3.0 * std::tan(span / 4.0);
      P.segs.push_back(sg);
    }
  }
}

// merges legs shorter than their ends need: a smooth point dropped (its two legs one), two junctions one, a stub at a
// free end removed
void tidy(Plan& P) {
  for (int round = 0; round < 40; ++round) {
    Geo G = arms(P);
    std::vector<double> R(P.nodes.size(), -1);
    auto radius = [&](int n) {
      if (R[size_t(n)] < 0) R[size_t(n)] = !P.nodes[size_t(n)].track && junction(P, G, n) ? junctionShape(P, G, n).radius : 0.0;
      return R[size_t(n)];
    };
    std::vector<std::pair<double, int>> order;
    for (size_t i = 0; i < P.segs.size(); ++i)
      if (P.segs[i].alive) {
        const Node &A = P.nodes[size_t(P.segs[i].a)], &B = P.nodes[size_t(P.segs[i].b)];
        order.push_back({std::hypot(B.x - A.x, B.z - A.z), int(i)});
      }
    std::sort(order.begin(), order.end());
    std::vector<char> touched(P.nodes.size(), 0);
    bool changed = false;
    for (auto [len, si] : order) {
      Seg& s = P.segs[size_t(si)];
      if (!s.alive || s.fixed || touched[size_t(s.a)] || touched[size_t(s.b)]) continue;
      double ra = radius(s.a), rb = radius(s.b);
      double need = std::max(minLen(s.kind), (ra > 0 ? ra + 1.0 : 0.0) + (rb > 0 ? rb + 1.0 : 0.0) + 1.0);
      if (len >= need) continue;
      // drop a smooth end (a point of the way), joining its two legs
      bool done = false;
      for (int n : {s.a, s.b}) {
        if (done || !smooth(P, G, n)) continue;
        const auto& as = G.arms[size_t(n)];
        Seg& s1 = P.segs[size_t(as[0])];
        Seg& s2 = P.segs[size_t(as[1])];
        if (s1.kind != s2.kind || s1.flags != s2.flags || s1.bridge != s2.bridge) continue;
        // s1 runs into n, s2 out of it (one-way streets keep their way)
        Seg* in = &s1;
        Seg* out = &s2;
        if (in->b != n) std::swap(in, out);
        if (in->b != n || out->a != n) {
          if (s1.flags & 0x20) continue;
          if (s1.b != n) std::swap(s1.a, s1.b);
          if (s2.a != n) std::swap(s2.a, s2.b);
          in = &s1, out = &s2;
        }
        if (in->a == out->b) continue;  // a loop of two legs
        in->b = out->b;
        out->alive = false;
        P.nodes[size_t(n)].alive = false;
        touched[size_t(n)] = touched[size_t(in->a)] = touched[size_t(in->b)] = 1;
        done = changed = true;
        ++P.merged;
      }
      if (done) continue;
      bool ja = ra > 0, jb = rb > 0;
      const Node &A = P.nodes[size_t(s.a)], &B = P.nodes[size_t(s.b)];
      if (ja && jb && A.exit < 0 && B.exit < 0 && !(A.fixed && B.fixed)) {  // two junctions: one in the middle
        int keepN = B.fixed ? s.b : s.a, gone = B.fixed ? s.a : s.b;  // a roundabout's point stays where it is
        Node& K = P.nodes[size_t(keepN)];
        if (!K.fixed) K.x = (A.x + B.x) * 0.5, K.z = (A.z + B.z) * 0.5;
        K.corner = false;
        for (Seg& o : P.segs) {
          if (!o.alive) continue;
          if (o.a == gone) o.a = keepN;
          if (o.b == gone) o.b = keepN;
        }
        s.alive = false;
        P.nodes[size_t(gone)].alive = false;
        // no leg from the point to itself, no two legs between the same points
        std::set<std::pair<int, int>> pairs;
        for (Seg& o : P.segs) {
          if (!o.alive) continue;
          if (o.a == o.b) {
            o.alive = false;
            continue;
          }
          auto k = std::minmax(o.a, o.b);
          if (!pairs.insert({k.first, k.second}).second) o.alive = false;
        }
        touched[size_t(keepN)] = 1;
        changed = true;
        ++P.merged;
        continue;
      }
      // a short stub (a free end, an edge stub's leg, or nothing left to merge): the leg goes
      if (std::getenv("REGS_DEBUG_OSM"))
        LOG_INFO("streets: leg dropped at (%.0f,%.0f)-(%.0f,%.0f) kind %d len %.1f need %.1f, arms %zu/%zu, radius %.1f/%.1f, exit %d/%d",
                 A.x, A.z, B.x, B.z, s.kind, len, need, G.arms[size_t(s.a)].size(), G.arms[size_t(s.b)].size(), ra, rb, A.exit, B.exit);
      s.alive = false;
      touched[size_t(s.a)] = touched[size_t(s.b)] = 1;
      changed = true;
      ++P.dropped;
    }
    if (!changed) break;
  }
  Geo G = arms(P);
  for (size_t n = 0; n < P.nodes.size(); ++n)
    if (G.arms[n].empty()) P.nodes[n].alive = false;
  for (Exit& e : P.exits)
    if (e.node >= 0 && !P.nodes[size_t(e.node)].alive) e.node = -1;
}

// the curve of a leg: tangents from the neighbours (smooth points), the arms' spread directions (junctions), the
// chord (free ends), the stub's axis (edge ends)
CurveRoad curveOf(const Plan& P, const Geo& G, const std::map<int, JunctionShape>& J, int si) {
  const Seg& s = P.segs[size_t(si)];
  const Node &A = P.nodes[size_t(s.a)], &B = P.nodes[size_t(s.b)];
  if (s.fixed) {  // a roundabout's arc
    CurveRoad c;
    c.kind = s.kind;
    c.x0 = float(A.x), c.y0 = A.y, c.z0 = float(A.z), c.dx0 = float(s.d0x), c.dz0 = float(s.d0z);
    c.x1 = float(B.x), c.y1 = B.y, c.z1 = float(B.z), c.dx1 = float(s.d1x), c.dz1 = float(s.d1z);
    c.a = c.b = float(s.h);
    c.ends = s.flags;
    return c;
  }
  auto away = [&](int n, double& dx, double& dz) {  // the direction at n into the leg
    const Node& N = P.nodes[size_t(n)];
    int o = otherEnd(s, n);
    if (N.exit >= 0) {
      dx = P.exits[size_t(N.exit)].dx, dz = P.exits[size_t(N.exit)].dz;
      return;
    }
    auto j = J.find(n);
    if (j != J.end()) {
      auto d = j->second.dir.find(si);
      if (d != j->second.dir.end()) {
        dx = d->second.first, dz = d->second.second;
        return;
      }
    }
    if (smooth(P, G, n)) {
      const auto& as = G.arms[size_t(n)];
      int other = otherEnd(P.segs[size_t(as[0] == si ? as[1] : as[0])], n);
      chordDir(P, other, o, dx, dz);
      return;
    }
    if (N.track && G.arms[size_t(n)].size() >= 2) {  // a track's branch: on into the straightest other arm
      double mx, mz, best = 2;
      int partner = -1;
      chordDir(P, n, o, mx, mz);
      for (int other : G.arms[size_t(n)]) {
        if (other == si) continue;
        double ox, oz;
        chordDir(P, n, otherEnd(P.segs[size_t(other)], n), ox, oz);
        double dd = mx * ox + mz * oz;
        if (dd < best) best = dd, partner = otherEnd(P.segs[size_t(other)], n);
      }
      if (partner >= 0 && best < -0.5) {
        chordDir(P, partner, o, dx, dz);
        return;
      }
    }
    chordDir(P, n, o, dx, dz);
  };
  double d0x, d0z, d1x, d1z;
  away(s.a, d0x, d0z);
  away(s.b, d1x, d1z);
  double len = std::hypot(B.x - A.x, B.z - A.z);
  CurveRoad c;
  c.kind = s.kind;
  c.x0 = float(A.x), c.y0 = A.y, c.z0 = float(A.z), c.dx0 = float(d0x), c.dz0 = float(d0z);
  c.x1 = float(B.x), c.y1 = B.y, c.z1 = float(B.z), c.dx1 = float(-d1x), c.dz1 = float(-d1z);
  c.a = c.b = std::max(0.5f, float(len / 3.0));
  c.ends = s.flags;
  c.bridge = s.bridge ? 1 : 0;
  if (A.exit >= 0) c.ends |= 1;
  if (B.exit >= 0) c.ends |= 2;
  return c;
}

// tidies the graph and builds its curves; bends tighter than the kind allows become junctions (or the leg goes)
std::vector<CurveRoad> curves(Plan& P) {
  std::vector<CurveRoad> out;
  for (int round = 0; round < 5; ++round) {
    tidy(P);
    Geo G = arms(P);
    std::map<int, JunctionShape> J;
    for (size_t n = 0; n < P.nodes.size(); ++n)
      if (P.nodes[n].alive && !P.nodes[n].track && junction(P, G, int(n))) J[int(n)] = junctionShape(P, G, int(n));
    out.clear();
    bool again = false;
    for (size_t i = 0; i < P.segs.size(); ++i) {
      if (!P.segs[i].alive) continue;
      CurveRoad c = curveOf(P, G, J, int(i));
      const CurveProfile& pf = curveProfile(c.kind);
      if (curveMinRadius(c) >= pf.minRadius - 0.05f && curveLength(c) >= float(minLen(c.kind)) - 0.01f) {
        out.push_back(c);
        continue;
      }
      if (P.segs[i].fixed || curveIsTrack(P.segs[i].kind)) {  // a roundabout's arc stays; tracks keep their line
        out.push_back(c);
        continue;
      }
      bool marked = false;
      for (int n : {P.segs[i].a, P.segs[i].b})
        if (smooth(P, G, n)) P.nodes[size_t(n)].corner = marked = true, ++P.corners;
      if (marked && round < 4) again = true;
      else if (round == 4 || !marked) {
        if (std::getenv("REGS_DEBUG_OSM"))
          LOG_INFO("streets: curve dropped at (%.0f,%.0f)-(%.0f,%.0f) kind %d radius %.1f length %.1f", c.x0, c.z0, c.x1, c.z1, c.kind,
                   curveMinRadius(c), curveLength(c));
        P.segs[i].alive = false, ++P.dropped, again = again || round < 4;
      }
    }
    if (!again) break;
  }
  return out;
}

}  // namespace streets

// the terrain's height at a world point (bilinear over the corners), in world units
float groundY(const GridData& g, double wx, double wz) {
  const float cs = g.cellSize;
  double cx = wx / cs + g.width * 0.5, cz = wz / cs + g.length * 0.5;
  int x = std::clamp(int(std::floor(cx)), 0, g.width - 2), z = std::clamp(int(std::floor(cz)), 0, g.length - 2);
  double fx = std::clamp(cx - x, 0.0, 1.0), fz = std::clamp(cz - z, 0.0, 1.0);
  auto H = [&](int xx, int zz) { return double(g.height[size_t(zz) * size_t(g.width) + size_t(xx)]); };
  double h = (H(x, z) * (1 - fx) + H(x + 1, z) * fx) * (1 - fz) + (H(x, z + 1) * (1 - fx) + H(x + 1, z + 1) * fx) * fz;
  return float(h * cs / 256.0);
}

// all the import's curves at once: the map's curve list, the terrain under them and their junctions levelled, one
// redraw (the curve tool's applyCurveEdit per curve looks for the junctions each time)
void addCurvesBulk(VM& vm, GridObj* grid, Object* tl, const std::vector<CurveRoad>& cs) {
  GridData& g = *grid->data;
  gridEditBegin(vm, grid);
  // the corners of water cells keep their height (levelling under junctions and road ends at a bank would lift the
  // river bed into view: a brown patch at a bridge's end)
  std::vector<std::pair<size_t, int16_t>> wetCorners;
  if (!g.height.empty())
    for (int z = 0; z < g.length; ++z)
      for (int x = 0; x < g.width; ++x) {
        bool wet = false;
        for (int dz = -1; dz <= 0 && !wet; ++dz)
          for (int dx = -1; dx <= 0 && !wet; ++dx) {
            int cx = x + dx, cz = z + dz;
            wet = cx >= 0 && cz >= 0 && cx < g.width && cz < g.length && (g.flags[size_t(cz) * size_t(g.width) + size_t(cx)] & 0x2000);
          }
        if (wet) wetCorners.push_back({size_t(z) * size_t(g.width) + size_t(x), g.height[size_t(z) * size_t(g.width) + size_t(x)]});
      }
  for (const CurveRoad& c : cs) g.curves.push_back(c);
  const float cell = g.cellSize;
  // the ground beside the streets: flat at their height 3 units beyond the edge, easing out over 6 more (the nearest
  // street wins), so slopes across a street do not poke through its pavement (real terrain is hillier than what the
  // curve tool's own levelling meets when a road is drawn by hand)
  if (!g.height.empty()) {
    const size_t N = size_t(g.width) * size_t(g.length);
    std::vector<float> bestD(N, 1e9f), bestT(N, 0.0f);
    for (const CurveRoad& c : cs) {
      if (c.bridge) continue;  // a bridge leaves the ground under it
      const float hw = curveProfile(c.kind).halfWidth;
      std::vector<CurveSample> shape = curveShape(c);
      int x0, z0, x1, z1;
      curveCells(g, c, x0, z0, x1, z1);
      x0 = std::max(0, x0 - 3), z0 = std::max(0, z0 - 3), x1 = std::min(g.width - 1, x1 + 4), z1 = std::min(g.length - 1, z1 + 4);
      for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
          size_t ci = size_t(z) * size_t(g.width) + size_t(x);
          float wx = (float(x) - float(g.width) * 0.5f) * cell, wz = (float(z) - float(g.length) * 0.5f) * cell;
          float side, sAt;
          curveNearest(shape, wx, wz, side, sAt);
          float ad = std::fabs(side) - hw;
          if (ad > 9.0f || ad >= bestD[ci]) continue;
          bestD[ci] = ad, bestT[ci] = (curveAt(shape, sAt).p.y - 0.25f) * 256.0f / cell;
        }
    }
    auto wetCorner = [&](int x, int z) {
      for (int dz = -1; dz <= 0; ++dz)
        for (int dx = -1; dx <= 0; ++dx) {
          int cx = x + dx, cz = z + dz;
          if (cx >= 0 && cz >= 0 && cx < g.width && cz < g.length && (g.flags[size_t(cz) * size_t(g.width) + size_t(cx)] & 0x2000)) return true;
        }
      return false;
    };
    for (int z = 0; z < g.length; ++z)
      for (int x = 0; x < g.width; ++x) {
        size_t ci = size_t(z) * size_t(g.width) + size_t(x);
        if (bestD[ci] > 9.0f || (g.flags[ci] & 0x1000) || wetCorner(x, z)) continue;
        float w = bestD[ci] <= 3.0f ? 1.0f : 1.0f - (bestD[ci] - 3.0f) / 6.0f;
        w = w * w * (3.0f - 2.0f * w);
        float h = float(g.height[ci]) + (bestT[ci] - float(g.height[ci])) * w;
        g.height[ci] = int16_t(std::clamp(int(std::lround(h)), -0x7fff, 0x7fff));
      }
  }
  for (const CurveRoad& c : cs) levelTerrainUnderCurve(grid, c);
  for (const CurveJunction& j : findCurveJunctions(g)) {
    std::vector<render::Vec3> outer = junctionOutline(j, 1, 0.0f, 6);
    int x0, z0, x1, z1;
    junctionCells(g, j, x0, z0, x1, z1);
    int16_t target = int16_t(std::lround((j.centre.y - 0.25f) * 256.0f / cell));
    for (int z = z0; z <= std::min(z1 + 1, g.length - 1); ++z)
      for (int x = x0; x <= std::min(x1 + 1, g.width - 1); ++x) {
        size_t ci = size_t(z) * size_t(g.width) + size_t(x);
        if (g.flags[ci] & 0x1000) continue;
        float wx = (float(x) - float(g.width) * 0.5f) * cell, wz = (float(z) - float(g.length) * 0.5f) * cell;
        if (insideOutline(outer, wx, wz)) g.height[ci] = target;
      }
  }
  for (auto [i, h] : wetCorners) g.height[i] = h;
  refreshCurveAnchors(g);
  grid->curveRects.push_back({0, 0, g.width - 1, g.length - 1});
  grid->curvesDirty = true;
  ++grid->curvesVersion;
  gridEditCellsChanged(grid, 0, 0, g.width - 1, g.length - 1, true);
  gridEditEnd(vm, grid);
  if (tl)
    for (const CurveRoad& c : cs) timelineRecordCurve(vm, tl, c, true);
}

// debugging: how far the ground pokes above the pavement within a street's width (world units)
float groundAbovePavement(const GridData& g, const std::vector<CurveRoad>& cs, int& worst) {
  float m = -1e9f;
  worst = -1;
  const float cell = g.cellSize;
  for (size_t i = 0; i < cs.size(); ++i) {
    const CurveRoad& c = cs[i];
    const float hw = curveProfile(c.kind).halfWidth;
    std::vector<CurveSample> shape = curveShape(c);
    int x0, z0, x1, z1;
    curveCells(g, c, x0, z0, x1, z1);
    for (int z = z0; z <= z1; ++z)
      for (int x = x0; x <= x1; ++x) {
        float wx = (float(x) - float(g.width) * 0.5f) * cell, wz = (float(z) - float(g.length) * 0.5f) * cell;
        float side, sAt;
        curveNearest(shape, wx, wz, side, sAt);
        if (std::fabs(side) > hw || sAt <= 0.01f || sAt >= shape.back().s - 0.01f) continue;
        float d = float(g.height[size_t(z) * size_t(g.width) + size_t(x)]) * cell / 256.0f - curveAt(shape, sAt).p.y;
        if (d > m) m = d, worst = int(i);
      }
  }
  return m;
}

// the cells the curves take (buildings and trees keep off them)
void markCurveCells(const GridData& g, const std::vector<CurveRoad>& cs, std::vector<uint8_t>& used) {
  const float cell = g.cellSize;
  for (const CurveRoad& c : cs) {
    float r = curveProfile(c.kind).halfWidth + 1.5f;
    for (const CurveSample& s : sampleCurve(c, 1.5f)) {
      int x0 = int(std::floor((s.p.x - r) / cell + g.width * 0.5f)), x1 = int(std::floor((s.p.x + r) / cell + g.width * 0.5f));
      int z0 = int(std::floor((s.p.z - r) / cell + g.length * 0.5f)), z1 = int(std::floor((s.p.z + r) / cell + g.length * 0.5f));
      for (int z = std::max(0, z0); z <= std::min(g.length - 1, z1); ++z)
        for (int x = std::max(0, x0); x <= std::min(g.width - 1, x1); ++x) {
          float wx = (float(x) + 0.5f - g.width * 0.5f) * cell - s.p.x, wz = (float(z) + 0.5f - g.length * 0.5f) * cell - s.p.z;
          if (wx * wx + wz * wz <= (r + 1.0f) * (r + 1.0f)) used[size_t(z) * size_t(g.width) + size_t(x)] = 2;
        }
    }
  }
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

// the streets as points every 2 units with their direction (cells), for the buildings to line up along them
struct StreetIndex {
  struct P {
    float x, z, tx, tz, hw;  // cells
  };
  std::vector<P> pts;
  std::map<std::pair<int, int>, std::vector<uint32_t>> bucket;  // 8 × 8 cells
  void build(const GridData& g) {
    pts.clear(), bucket.clear();
    const float cs = g.cellSize;
    for (const CurveRoad& c : g.curves) {
      if (c.kind == 3 || curveIsTrack(c.kind)) continue;  // footpaths and tracks: houses face the streets
      const float hw = curveProfile(c.kind).halfWidth / cs;
      for (const CurveSample& sm : sampleCurve(c, 2.0f)) {
        float l = std::hypot(sm.dir.x, sm.dir.z);
        if (l < 1e-4f) continue;
        P q{sm.p.x / cs + g.width * 0.5f, sm.p.z / cs + g.length * 0.5f, sm.dir.x / l, sm.dir.z / l, hw};
        bucket[{int(std::floor(q.x / 8)), int(std::floor(q.z / 8))}].push_back(uint32_t(pts.size()));
        pts.push_back(q);
      }
    }
  }
  // the nearest street point within r cells of (x, z) (by the distance to its edge)
  const P* nearest(double x, double z, double r) const {
    const P* best = nullptr;
    double bd = r;
    for (int bz = int(std::floor((z - r) / 8)); bz <= int(std::floor((z + r) / 8)); ++bz)
      for (int bx = int(std::floor((x - r) / 8)); bx <= int(std::floor((x + r) / 8)); ++bx) {
        auto it = bucket.find({bx, bz});
        if (it == bucket.end()) continue;
        for (uint32_t i : it->second) {
          const P& q = pts[i];
          double d = std::hypot(q.x - x, q.z - z) - q.hw;
          if (d < bd) bd = d, best = &q;
        }
      }
    return best;
  }
};
StreetIndex g_streets;

// a street corner near (x, z): two streets within r cells running at 45° or more to each other
bool streetCorner(double x, double z, double r) {
  std::vector<const StreetIndex::P*> near;
  for (int bz = int(std::floor((z - r) / 8)); bz <= int(std::floor((z + r) / 8)); ++bz)
    for (int bx = int(std::floor((x - r) / 8)); bx <= int(std::floor((x + r) / 8)); ++bx) {
      auto it = g_streets.bucket.find({bx, bz});
      if (it == g_streets.bucket.end()) continue;
      for (uint32_t i : it->second) {
        const StreetIndex::P& q = g_streets.pts[i];
        if (std::hypot(q.x - x, q.z - z) - q.hw <= r) near.push_back(&q);
      }
    }
  for (size_t i = 0; i < near.size(); ++i)
    for (size_t j = i + 1; j < near.size(); ++j)
      if (std::fabs(near[i]->tx * near[j]->tz - near[i]->tz * near[j]->tx) > 0.7071f) return true;
  return false;
}

// the minimum-area rectangle of points (cells): its angle (of the side ea), the side lengths and the centre
bool minRect(std::vector<std::pair<double, double>> pts, double& ang, double& ea, double& eb, double& ccx, double& ccz) {
  std::sort(pts.begin(), pts.end());
  pts.erase(std::unique(pts.begin(), pts.end()), pts.end());
  if (pts.size() < 3) return false;
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
  if (hull.size() < 3) return false;
  double bestArea = 1e30;
  for (size_t i = 0; i < hull.size(); ++i) {
    auto p = hull[i], q = hull[(i + 1) % hull.size()];
    double t = std::atan2(q.second - p.second, q.first - p.first), c = std::cos(t), s = std::sin(t);
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
  return true;
}

double polyArea(const std::vector<std::pair<double, double>>& p) {
  double a = 0;
  for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) a += (p[j].first + p[i].first) * (p[j].second - p[i].second);
  return std::fabs(a) * 0.5;
}

// a row of buildings wall to wall (owner 2026-11-13: city houses stand in a row, joined): one straight row along its
// street, filled with the game's city houses edge to edge, fronts on the street-side line; a corner house at an end
// only where a second street crosses there. false: not a straight row along a street (its buildings go one by one)
bool placeRow(VM& vm, GridObj* grid, Apply& a, const std::vector<const BuildingIn*>& members, const Value& buildingsList,
              std::map<std::string, std::vector<Candidate>>& cache) {
  GridData& g = *grid->data;
  const int W = g.width, L = g.length;
  std::vector<std::pair<double, double>> all;
  double area = 0;
  for (const BuildingIn* b : members) all.insert(all.end(), b->pts.begin(), b->pts.end()), area += polyArea(b->pts);
  double ang, ea, eb, cx, cz;
  if (!minRect(all, ang, ea, eb, cx, cz)) return false;
  double ux = std::cos(ang), uz = std::sin(ang), len = ea, depth = eb;
  if (eb > ea) ux = -std::sin(ang), uz = std::cos(ang), len = eb, depth = ea;
  if (len < (members.size() >= 2 ? 1.2 : 1.8) * depth || area < 0.5 * len * depth || depth < 2.5) return false;  // not a straight row
  const StreetIndex::P* st = g_streets.nearest(cx, cz, depth * 0.5 + 5.0);
  if (!st || std::fabs(st->tx * uz - st->tz * ux) > 0.5) return false;  // no street along it (within 30°)
  if (st->tx * ux + st->tz * uz < 0) ux = -st->tx, uz = -st->tz;  // the street's direction exactly
  else ux = st->tx, uz = st->tz;
  double vx = -uz, vz = ux;
  double sgn = (st->x - cx) * vx + (st->z - cz) * vz > 0 ? 1.0 : -1.0;  // the street's side of the row
  // the cell turn: the nearest 90° and the rest as the free turn; the side the fronts face in cell terms
  const double quarter = kPi / 2, th = std::atan2(uz, ux);
  const double rem = th - std::round(th / quarter) * quarter;
  const double Ux = std::cos(rem), Uz = std::sin(rem), Vx = -Uz, Vz = Ux;
  double fx = vx * sgn, fz = vz * sgn, lu = fx * Ux + fz * Uz, lv = fx * Vx + fz * Vz;
  const int side = std::fabs(lu) > std::fabs(lv) ? (lu > 0 ? 1 : 3) : (lv > 0 ? 2 : 0);
  const int orient2 = side & 1;
  auto it = cache.find("blockhouses");
  if (it == cache.end()) it = cache.emplace("blockhouses", categoryTemplates(vm, grid, buildingsList, "blockhouses")).first;
  // the original maps (research 2026-11-13, item dumps of Berlin, Amsterdam, Helsinki): city houses have their street on
  // model -Z; the corner houses 01, 02, 05, 06 on -Z and +X, 03, 04 and small-townbuilding04 on -Z and -X
  std::vector<const Candidate*> houses, cornersPlusX, cornersMinusX;
  for (const Candidate& c : it->second)
    if (c.name == "cornerhouse03" || c.name == "cornerhouse04" || c.name == "small-townbuilding04") cornersMinusX.push_back(&c);
    else if (c.name.rfind("cornerhouse", 0) == 0) cornersPlusX.push_back(&c);
    else if (c.name.rfind("cityhouse", 0) == 0 || c.name.rfind("small-townbuilding", 0) == 0) houses.push_back(&c);
  if (houses.empty()) return false;
  // a house: w along the row, l deep (the template turned so); centred at along-row position t
  int placed = 0;
  auto put = [&](const Candidate& c, double t) {
    const int w = c.w, l = c.l;
    const int sx = orient2 ? l : w, sz = orient2 ? w : l;
    double off = sgn * (depth * 0.5 - l * 0.5);  // the front on the street-side line
    double px = cx + ux * t + vx * off, pz = cz + uz * t + vz * off;
    int x = int(std::lround(px - sx * 0.5)), z = int(std::lround(pz - sz * 0.5));
    // its cells: cell centres inside the turned rectangle (a sliver to spare: neighbours share a wall)
    std::vector<size_t> cells;
    double hx = sx * 0.5 - 0.15, hz = sz * 0.5 - 0.15, r = std::hypot(hx, hz);
    for (int zz = int(std::floor(pz - r)); zz <= int(std::ceil(pz + r)); ++zz)
      for (int xx = int(std::floor(px - r)); xx <= int(std::ceil(px + r)); ++xx) {
        double dx = xx + 0.5 - px, dz = zz + 0.5 - pz;
        if (std::fabs(dx * Ux + dz * Uz) > hx || std::fabs(dx * Vx + dz * Vz) > hz) continue;
        if (xx < 1 || zz < 1 || xx >= W - 1 || zz >= L - 1) return false;
        size_t ci = size_t(zz) * size_t(W) + size_t(xx);
        if (a.used[ci]) return false;
        cells.push_back(ci);
      }
    int orient = frontOrient(side);
    if ((orient & 1) != orient2) orient = (orient + 1) & 3;
    int made = addItem(vm, grid, a, c.name, x, maxCorner(g, x, z, sx, sz), z, orient);
    if (made < 0) return false;
    GridData::Item& item = g.items[size_t(made)];
    const float half = g.cellSize * 0.5f;
    item.yaw = float(-rem);
    item.offX = std::clamp(float((px - (x + sx * 0.5)) * g.cellSize), -half, half);
    item.offZ = std::clamp(float((pz - (z + sz * 0.5)) * g.cellSize), -half, half);
    for (size_t ci : cells) a.used[ci] = 3;
    ++a.buildingsPlaced;
    ++placed;
    return true;
  };
  uint32_t h = hash32(members.front()->id);
  double from = -len * 0.5, to = len * 0.5;
  // corner houses where a second street crosses at an end
  for (int end = 0; end < 2; ++end) {
    double t = end ? to : from;
    if (!streetCorner(cx + ux * t + vx * sgn * depth * 0.5, cz + uz * t + vz * sgn * depth * 0.5, 4.0)) continue;
    // the model +X side: the front (towards the street) turned 90° clockwise seen from above
    double fx = vx * sgn, fz = vz * sgn, px = -fz, pz = fx, ex = end ? ux : -ux, ez = end ? uz : -uz;
    const auto& corners = ex * px + ez * pz > 0 ? cornersPlusX : cornersMinusX;  // the corner on the end's side
    if (corners.empty()) continue;
    const Candidate& c = *corners[(h >> (4 * end)) % corners.size()];
    if (c.w > to - from) continue;
    if (put(c, end ? to - c.w * 0.5 : from + c.w * 0.5)) {
      if (end) to -= c.w;
      else from += c.w;
    }
  }
  // the houses between, edge to edge, the run centred
  std::vector<const Candidate*> run;
  double used = 0;
  int minW = 1 << 30;
  for (const Candidate* c : houses) minW = std::min(minW, c->w);
  for (int k = 0; used + minW <= to - from + 0.3 && k < 64; ++k) {
    const Candidate* pick = nullptr;
    for (int tries = 0; tries < 8 && !pick; ++tries) {
      const Candidate* c = houses[hash32(uint64_t(h) * 31 + uint64_t(k) * 7 + uint64_t(tries)) % houses.size()];
      if (used + c->w <= to - from + 0.3) pick = c;
    }
    if (!pick) break;
    run.push_back(pick);
    used += pick->w;
  }
  double t = from + (to - from - used) * 0.5;
  for (const Candidate* c : run) {
    put(*c, t + c->w * 0.5);
    t += c->w;
  }
  return placed > 0;
}

void placeBuildingAs(VM& vm, GridObj* grid, Apply& a, const BuildingIn& b, const Value& buildingsList,
                     std::map<std::string, std::vector<Candidate>>& cache, bool alongStreet) {
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
  // the street in front (owner 2026-11-13: buildings along the streets): the nearest street within the building's
  // half size and 4 cells; its direction (a multiple of 90° off it) turns the building when the footprint is within
  // 30° of it, so a street's houses stand in one line
  const StreetIndex::P* street = alongStreet ? g_streets.nearest(ccx, ccz, std::max(ea, eb) * 0.5 + 4.0) : nullptr;
  if (street) {
    double ts = std::atan2(double(street->tz), double(street->tx));
    double k = std::round((ang - ts) / (kPi / 2));
    double snapped = ts + k * (kPi / 2);
    if (std::fabs(snapped - ang) < 30.0 * kPi / 180.0) ang = snapped;
  }
  // the rectangle laid on the cell axes: its sides along x and z
  double deg = std::fmod(std::abs(ang * 180.0 / kPi), 180.0);
  bool swap = deg > 45.0 && deg < 135.0;
  double ex = swap ? eb : ea, ez = swap ? ea : eb;
  if (ex < 0.8 || ez < 0.8) return;
  // the turn left over from the nearest 90° (owner 2026-11-08: buildings at their footprint's angle; the port's free
  // turn, GridData::Item::yaw): the rectangle's axes u (its x side) and v (its z side)
  const double quarter = kPi / 2;
  double rem = ang - std::round(ang / quarter) * quarter;
  static const bool noTurn = std::getenv("REGS_OSM_NOTURN") != nullptr;
  if (noTurn || std::fabs(rem) < 1.0 * kPi / 180.0) rem = 0;
  const double ux = std::cos(rem), uz = std::sin(rem), vx = -uz, vz = ux;
  // the side facing the street: 0 north (−v), 1 east (+u), 2 south (+v), 3 west (−u)
  int streetSide = -1;
  if (street) {
    double dx = street->x - ccx, dz = street->z - ccz, lu = dx * ux + dz * uz, lv = dx * vx + dz * vz;
    streetSide = std::fabs(lu) > std::fabs(lv) ? (lu > 0 ? 1 : 3) : (lv > 0 ? 2 : 0);
  }
  // candidates of the category (or its neighbours)
  static const std::map<std::string, std::vector<std::string>> kFallback = {
      {"residential", {"residential", "blockhouses"}}, {"blockhouses", {"blockhouses", "residential"}},
      {"commercial", {"commercial", "workplace", "blockhouses"}}, {"workplace", {"workplace", "commercial"}},
      {"industry", {"industry", "workplace"}}, {"government", {"government", "workplace", "commercial"}},
      {"farm", {"farm", "residential"}}, {"railway", {"railway", "industry"}}};
  std::vector<Candidate> cands;
  if (b.terraced) {  // the city houses (and corner houses for squarish footprints)
    auto it = cache.find("blockhouses");
    if (it == cache.end()) it = cache.emplace("blockhouses", categoryTemplates(vm, grid, buildingsList, "blockhouses")).first;
    const bool corner = std::min(ex, ez) >= 5.5 && std::max(ex, ez) < 1.4 * std::min(ex, ez) &&
                        streetCorner(ccx, ccz, std::max(ex, ez) * 0.5 + 3.0);  // only at a street corner
    for (const Candidate& c : it->second)
      if (corner ? (c.name.rfind("cornerhouse", 0) == 0 || c.name == "small-townbuilding04")
                 : (c.name.rfind("cityhouse", 0) == 0 || (c.name.rfind("small-townbuilding", 0) == 0 && c.name != "small-townbuilding04")))
        cands.push_back(c);
    // too small a footprint for any of them (a single house wall to wall with its garage): the usual buildings
    bool fits = false;
    for (const Candidate& c : cands)
      fits = fits || (c.w <= ex + 0.6 && c.l <= ez + 0.6) || (c.l <= ex + 0.6 && c.w <= ez + 0.6);
    if (!fits) cands.clear();
  }
  auto fb = kFallback.find(b.category);
  for (const std::string& c : fb == kFallback.end() ? std::vector<std::string>{b.category, "residential"} : fb->second) {
    if (!cands.empty()) break;
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
  if (streetSide >= 0) {  // turned so that the front can face the street (orient2 0: north/south, 1: east/west)
    std::vector<const Fit*> facing;
    for (const Fit* f : good)
      if (f->orient2 == (streetSide & 1)) facing.push_back(f);
    if (facing.empty())
      for (auto& f : fits)
        if (f.orient2 == (streetSide & 1) && f.score >= best * 0.6) facing.push_back(&f);
    if (!facing.empty()) good.swap(facing);
  }
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
  if (chosen.score < (b.terraced ? 0.7 : 0.45) && b.polygon) {
    nx = std::max(1, int(ex / chosen.sx)), nz = std::max(1, int(ez / chosen.sz));
    while (nx * nz > 16) nx > nz ? --nx : --nz;
    if (streetSide >= 0 && b.terraced) (streetSide & 1) ? nx = 1 : nz = 1;  // one row along the street
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
  // the cells under a turned rectangle sx × sz centred at (cx, cz) (cell centres inside it, a little to spare)
  auto turnedCells = [&](double cx, double cz, int sx, int sz, std::vector<size_t>& out) {
    out.clear();
    const double spare = b.terraced ? -0.15 : 0.2;  // a row's houses stand wall to wall
    double hx = sx * 0.5 + spare, hz = sz * 0.5 + spare, r = std::hypot(hx, hz);
    for (int z = int(std::floor(cz - r)); z <= int(std::ceil(cz + r)); ++z)
      for (int x = int(std::floor(cx - r)); x <= int(std::ceil(cx + r)); ++x) {
        double dx = x + 0.5 - cx, dz = z + 0.5 - cz;
        double lu = dx * ux + dz * uz, lv = dx * vx + dz * vz;
        if (std::fabs(lu) > hx || std::fabs(lv) > hz) continue;
        if (x < 1 || z < 1 || x >= W - 1 || z >= L - 1) return false;
        size_t i = size_t(z) * size_t(W) + size_t(x);
        if (a.used[i]) return false;
        out.push_back(i);
      }
    return true;
  };
  std::vector<size_t> cells;
  auto roadAt = [&](int x, int z) {
    return x >= 0 && z >= 0 && x < W && z < L && a.used[size_t(z) * size_t(W) + size_t(x)] == 2;
  };
  double stepX = ex / nx, stepZ = ez / nz;
  // a single depth: the front on the footprint's street-side wall (the street's building line)
  double frontU = 0, frontV = 0;
  if (streetSide == 1 || streetSide == 3) {
    if (nx == 1) frontU = std::max(0.0, (ex - chosen.sx) * 0.5) * (streetSide == 1 ? 1 : -1);
  } else if (streetSide >= 0 && nz == 1) {
    frontV = std::max(0.0, (ez - chosen.sz) * 0.5) * (streetSide == 2 ? 1 : -1);
  }
  for (int iz = 0; iz < nz; ++iz)
    for (int ix = 0; ix < nx; ++ix) {
      bool placed = false;
      for (int attempt = 0; attempt < 2 && !placed; ++attempt) {  // on the building line, else where OSM has it
      if (attempt && frontU == 0 && frontV == 0) break;
      double lx = -ex * 0.5 + stepX * (ix + 0.5) + (attempt ? 0 : frontU), lz = -ez * 0.5 + stepZ * (iz + 0.5) + (attempt ? 0 : frontV);
      double px = ccx + lx * ux + lz * vx, pz = ccz + lx * uz + lz * vz;
      if (nx * nz > 1 && !insidePoly(px, pz)) continue;
      const Candidate& c = *chosen.c;
      int sx = chosen.sx, sz = chosen.sz;
      int bx = int(std::lround(px - sx * 0.5)), bz = int(std::lround(pz - sz * 0.5));
      for (int r = 0; r <= 2 && !placed; ++r)
        for (int dz = -r; dz <= r && !placed; ++dz)
          for (int dx = -r; dx <= r && !placed; ++dx) {
            if (std::max(std::abs(dx), std::abs(dz)) != r) continue;
            int x = bx + dx, z = bz + dz;
            if (rem == 0 ? !freeRect(x, z, sx, sz) : !turnedCells(px + dx, pz + dz, sx, sz, cells)) continue;
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
            if (streetSide >= 0 && (streetSide & 1) == (sideA & 1)) side = streetSide;  // the front to the street
            int orient = frontOrient(side);
            if ((orient & 1) != chosen.orient2) orient = (orient + 1) & 3;  // the table must keep the footprint
            int y = maxCorner(g, x, z, sx, sz);
            int made = addItem(vm, grid, a, c.name, x, y, z, orient);
            if (made >= 0) {
              if (rem != 0) {  // turned about its footprint's centre, shifted onto the OSM one (at most half a cell)
                GridData::Item& it = g.items[size_t(made)];
                it.yaw = float(-rem);
                const float half = g.cellSize * 0.5f;
                it.offX = std::clamp(float((px + dx - (x + sx * 0.5)) * g.cellSize), -half, half);
                it.offZ = std::clamp(float((pz + dz - (z + sz * 0.5)) * g.cellSize), -half, half);
                for (size_t ci : cells) a.used[ci] = 3;
              } else {
                for (int zz = z; zz < z + sz; ++zz)
                  for (int xx = x; xx < x + sx; ++xx) a.used[size_t(zz) * size_t(W) + size_t(xx)] = 3;
              }
              ++a.buildingsPlaced;
              placed = true;
            }
          }
      }
    }
}

// the railway stations (owner 2026-11-13): the game's railway-station (14 × 5, its own two platform tracks along its −Z
// side, 8 cells deep) parallel to the nearest railway, its tracks beside the main line, on the station node's side
void placeStations(VM& vm, GridObj* grid, Apply& a, const std::vector<StationIn>& stations) {
  GridData& g = *grid->data;
  const int W = g.width, L = g.length;
  const std::string name = "railway-station";
  if (stations.empty() || !grid->templates.count(name)) return;
  const float cs = g.cellSize;
  struct RP {
    double x, z, tx, tz;
  };
  std::vector<RP> rail;
  for (const CurveRoad& c : g.curves) {
    if (c.kind != 6) continue;
    for (const CurveSample& sm : sampleCurve(c, 2.0f)) {
      double l = std::hypot(sm.dir.x, sm.dir.z);
      if (l > 1e-4) rail.push_back({sm.p.x / cs + W * 0.5, sm.p.z / cs + L * 0.5, sm.dir.x / l, sm.dir.z / l});
    }
  }
  const int w = 14, l = 5;
  for (const StationIn& st : stations) {
    const RP* best = nullptr;
    double bd = 20.0;
    for (const RP& r : rail) {
      double d = std::hypot(r.x - st.x, r.z - st.z);
      if (d < bd) bd = d, best = &r;
    }
    if (!best) continue;
    double ux = best->tx, uz = best->tz, vx = -uz, vz = ux;
    double sgn = (st.x - best->x) * vx + (st.z - best->z) * vz >= 0 ? 1.0 : -1.0;  // the station's side
    const double quarter = kPi / 2, th = std::atan2(uz, ux);
    const double rem = th - std::round(th / quarter) * quarter;
    const double Ux = std::cos(rem), Uz = std::sin(rem), Vx = -Uz, Vz = Ux;
    double fx = -vx * sgn, fz = -vz * sgn, lu = fx * Ux + fz * Uz, lv = fx * Vx + fz * Vz;  // the front: to the line
    const int side = std::fabs(lu) > std::fabs(lv) ? (lu > 0 ? 1 : 3) : (lv > 0 ? 2 : 0);
    const int orient2 = side & 1;
    const int sx = orient2 ? l : w, sz = orient2 ? w : l;
    const double off = 2.0 + 8.0 + l * 0.5;  // the main line's half width, the platform tracks, half the building
    double px = best->x + vx * sgn * off, pz = best->z + vz * sgn * off;
    int x = int(std::lround(px - sx * 0.5)), z = int(std::lround(pz - sz * 0.5));
    // the building's cells and the platform tracks' strip must be free
    std::vector<size_t> cells;
    bool free = true;
    double hx = w * 0.5, hzBack = l * 0.5, hzFront = l * 0.5 + 8.0;
    for (int zz = int(std::floor(pz - 16)); zz <= int(std::ceil(pz + 16)) && free; ++zz)
      for (int xx = int(std::floor(px - 16)); xx <= int(std::ceil(px + 16)) && free; ++xx) {
        double dx = xx + 0.5 - px, dz = zz + 0.5 - pz;
        double along = dx * ux + dz * uz, across = (dx * vx + dz * vz) * sgn;  // across > 0: away from the line
        if (std::fabs(along) > hx || across > hzBack || across < -hzFront) continue;
        if (xx < 1 || zz < 1 || xx >= W - 1 || zz >= L - 1) {
          free = false;
          break;
        }
        size_t ci = size_t(zz) * size_t(W) + size_t(xx);
        if (a.used[ci] == 3) free = false;  // buildings are in the way (trees, paving and the like give way)
        cells.push_back(ci);
      }
    if (!free) continue;
    int orient = frontOrient(side);
    if ((orient & 1) != orient2) orient = (orient + 1) & 3;
    int made = addItem(vm, grid, a, name, x, maxCorner(g, x, z, sx, sz), z, orient);
    if (made < 0) continue;
    GridData::Item& item = g.items[size_t(made)];
    const float half = cs * 0.5f;
    item.yaw = float(-rem);
    item.offX = std::clamp(float((px - (x + sx * 0.5)) * cs), -half, half);
    item.offZ = std::clamp(float((pz - (z + sz * 0.5)) * cs), -half, half);
    for (size_t ci : cells) a.used[ci] = 3;
    ++a.stationsPlaced;
    LOG_INFO("osm: station '%s' placed", st.name.c_str());
  }
}

// a building lined up along its street, else (nothing fits that way) as OSM has it
void placeBuilding(VM& vm, GridObj* grid, Apply& a, const BuildingIn& b, const Value& buildingsList,
                   std::map<std::string, std::vector<Candidate>>& cache) {
  if (b.category == "railway" && a.stationsPlaced) return;  // the station building stands at its track already
  int before = a.buildingsPlaced;
  placeBuildingAs(vm, grid, a, b, buildingsList, cache, true);
  if (a.buildingsPlaced == before) placeBuildingAs(vm, grid, a, b, buildingsList, cache, false);
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
    a.gridStreets = std::getenv("REGS_OSM_GRID") != nullptr;
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
    if (a.gridStreets) buildNetwork(d.roads, W, L, d.water, a.network, a.drags);
    bool water = (d.flags & kWater) != 0;
    if (!d.heights.empty() || water || !d.paving.empty()) {
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
          if (!d.paving.empty() && d.paving[i]) {  // the pavement paint (the terrain menu's tool)
            f = uint16_t((f & 0xFFD8) | ((d.paving[i] == 4 ? 2 : d.paving[i] - 1) & 7) | 0x8);
            if (d.paving[i] != 4) a.used[i] = 5;  // no trees or buildings on squares and car parks
          }
          g.flags[i] = f;
          flags[i] = uint16_t(f & kGround);
          if (d.water[i]) a.used[i] = 1;
        }
      // rounded river banks (owner 2026-11-13): the ground within 5 cells of the water eases down to just above the
      // water line, except where buildings stand at the water (a quay wall stays steep)
      if (!d.heights.empty() && water) {
        std::vector<uint8_t> built(size_t(W) * size_t(L), 0);
        for (const BuildingIn& b : d.buildings) {
          if (!b.polygon || b.pts.size() < 3) continue;
          Edges e;
          addRing(e, b.pts, true);
          fillEdges(e, W, L, [&](int x, int z) { built[size_t(z) * size_t(W) + size_t(x)] = 1; });
        }
        const int R = 5;
        const int target = -0x100;  // just above the water line (-0x140)
        auto wetCell = [&](int x, int z) { return x >= 0 && z >= 0 && x < W && z < L && d.water[size_t(z) * size_t(W) + size_t(x)]; };
        std::vector<int16_t> out(heights);
        for (int z = 0; z < L; ++z)
          for (int x = 0; x < W; ++x) {
            size_t i = size_t(z) * size_t(W) + size_t(x);
            if (wetCell(x - 1, z - 1) || wetCell(x, z - 1) || wetCell(x - 1, z) || wetCell(x, z)) continue;  // a water corner
            if (heights[i] <= target) continue;
            double best = R + 1;
            bool quay = false;
            for (int dz = -R; dz <= R; ++dz)
              for (int dx = -R; dx <= R; ++dx) {
                int cx = x + dx, cz = z + dz;
                if (cx < 0 || cz < 0 || cx >= W || cz >= L) continue;
                if (std::abs(dx) <= 2 && std::abs(dz) <= 2 && built[size_t(cz) * size_t(W) + size_t(cx)]) quay = true;
                if (d.water[size_t(cz) * size_t(W) + size_t(cx)]) best = std::min(best, std::hypot(dx + 0.5, dz + 0.5));
              }
            if (quay || best > R) continue;
            double t = std::clamp((best - 0.5) / (R - 0.5), 0.0, 1.0);
            t = t * t * (3 - 2 * t);
            out[i] = int16_t(std::lround(target + (heights[i] - target) * t));
          }
        heights.swap(out);
        g.height = heights;
      }
      if (!d.heights.empty()) levelUnderRoads(g, a.network, d.water, heights);
      gridEditCellsChanged(grid, 0, 0, W - 1, L - 1, true);
      if (a.timeline) timelineRecordPatch(vm, a.timeline, 0, 0, W, L, heights, flags);
    }
    a.stage = 2;
    a.next = 0;
    if (now() - t0 > budget) return false;
  }
  if (a.stage == 2 && !a.gridStreets && !d.roads.empty()) {  // the streets as curves
    streets::Plan plan = streets::plan(d.roads, W, L, g.cellSize, d.water);
    streets::rebuildRings(plan);
    streets::tidy(plan);
    // the map-edge stubs: the edge piece and a straight piece the curve starts on
    Blocks stubs;
    for (const streets::Exit& e : plan.exits) {
      if (e.node < 0) continue;
      const uint32_t in = e.side == 0 ? 4u : e.side == 1 ? 0x40u : e.side == 2 ? 0x10u : 1u;  // into the map
      const uint32_t outB = ((in << 4) | (in >> 4)) & 0xff;
      const int sh = e.type == 5 ? 8 : 0;
      stubs[{e.ex, e.ez}] |= in << sh;
      stubs[{e.bx, e.bz}] |= (in | outB) << sh;
    }
    if (!stubs.empty()) {
      std::vector<uint32_t> mounted;
      a.roadsOk = toolBuildRoadNetwork(vm, player, grid, std::move(stubs), mounted);
      for (uint32_t m : mounted) a.made.push_back(int(m));
    }
    for (streets::Exit& e : plan.exits) {
      if (e.node < 0) continue;
      streets::Node& n = plan.nodes[size_t(e.node)];
      bool found = false;
      for (const RoadEnd& re : findSmallRoadEnds(vm, player, grid, float(e.px), float(e.pz), 9.0f, e.type)) {
        if (re.deadEnd || re.tram || re.dir.x * float(e.dx) + re.dir.z * float(e.dz) < 0.9f) continue;
        n.x = re.point.x, n.z = re.point.z, n.y = re.point.y;
        found = true;
        break;
      }
      if (!found) n.exit = -1, e.node = -1;  // no stub there: a free end
    }
    for (streets::Node& n : plan.nodes)
      if (n.alive && n.exit < 0) n.y = groundY(g, n.x, n.z) + 0.2f;
    // bridge decks: at least their higher end, clear of the ground under them, 6 units over streets an OSM bridge
    // crosses; their end points lifted to the deck (the streets beyond ramp up to it)
    {
      auto crossesStreet = [&](const streets::Seg& b) {
        const streets::Node &A = plan.nodes[size_t(b.a)], &B = plan.nodes[size_t(b.b)];
        for (const streets::Seg& o : plan.segs) {
          if (!o.alive || o.bridge || o.a == b.a || o.a == b.b || o.b == b.a || o.b == b.b) continue;
          const streets::Node &C = plan.nodes[size_t(o.a)], &D = plan.nodes[size_t(o.b)];
          if (streets::segCross({A.x, A.z}, {B.x, B.z}, {C.x, C.z}, {D.x, D.z})) return true;
        }
        return false;
      };
      std::vector<float> deck(plan.nodes.size(), -1e9f);
      int bridges = 0;
      for (const streets::Seg& b : plan.segs) {
        if (!b.alive || !b.bridge) continue;
        const streets::Node &A = plan.nodes[size_t(b.a)], &B = plan.nodes[size_t(b.b)];
        float top = std::max(A.y, B.y), ground = -1e9f;
        for (int k = 1; k < 16; ++k) {
          double t = k / 16.0;
          ground = std::max(ground, groundY(g, A.x + (B.x - A.x) * t, A.z + (B.z - A.z) * t));
        }
        top = std::max(top, ground + 1.0f);
        if (b.osmBridge && crossesStreet(b)) top = std::max(top, ground + 6.0f);
        deck[size_t(b.a)] = std::max(deck[size_t(b.a)], top);
        deck[size_t(b.b)] = std::max(deck[size_t(b.b)], top);
        ++bridges;
      }
      for (size_t i = 0; i < plan.nodes.size(); ++i)
        if (plan.nodes[i].alive && plan.nodes[i].exit < 0 && deck[i] > plan.nodes[i].y) plan.nodes[i].y = deck[i];
      if (std::getenv("REGS_DEBUG_OSM")) LOG_INFO("osm: %d bridge legs", bridges);
    }
    std::vector<CurveRoad> cs = streets::curves(plan);
    addCurvesBulk(vm, grid, a.timeline, cs);
    markCurveCells(g, cs, a.used);
    if (std::getenv("REGS_DEBUG_OSM")) {
      int w;
      float up = groundAbovePavement(g, cs, w);
      LOG_INFO("osm: ground above pavement: %.2f (curve %d)", up, w);
    }
    a.curvesMade = int(cs.size());
    size_t exits = 0;
    for (const streets::Exit& e : plan.exits) exits += e.node >= 0;
    LOG_INFO("osm: streets: %zu curves, %zu edge stubs, %d legs merged, %d dropped, %d bends made junctions", cs.size(), exits,
             plan.merged, plan.dropped, plan.corners);
    if (std::getenv("REGS_DEBUG_CURVES")) LOG_INFO("osm: %zu curve junctions", findCurveJunctions(g).size());
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
    if (a.next == 0) cache.clear(), g_streets.build(g);
    if (a.next == 0 && std::getenv("REGS_DEBUG_OSM_BUILDINGS"))
      if (MapObj* m = vm.resolve(buildings).map())
        for (auto& [k, v] : m->entries) {
          std::string cat = utf16ToUtf8(vm.toStr(k)), all;
          for (const Candidate& c : categoryTemplates(vm, grid, buildings, cat)) all += c.name + "(" + std::to_string(c.w) + "x" + std::to_string(c.l) + ") ";
          LOG_INFO("osm: buildings '%s': %s", cat.c_str(), all.c_str());
        }
    gridEditBegin(vm, grid);
    if (a.next == 0) placeStations(vm, grid, a, d.stations);  // the stations before anything else takes their place
    if (a.next == 0) {  // the rows of city houses first; their buildings are done then
      // a row split by the street its buildings face (a block's rows turn at its corners): the nearest street's
      // direction in 20° steps
      std::map<std::pair<int, int>, std::vector<const BuildingIn*>> rows;
      for (const BuildingIn& b : d.buildings) {
        if (b.row < 0) continue;
        double cx = 0, cz = 0;
        for (auto& q : b.pts) cx += q.first, cz += q.second;
        cx /= double(b.pts.size()), cz /= double(b.pts.size());
        const StreetIndex::P* st = g_streets.nearest(cx, cz, 12.0);
        int dir = -1;
        if (st) {
          double t = std::atan2(double(st->tz), double(st->tx));
          if (t < 0) t += kPi;
          dir = int(std::lround(t / (kPi / 9))) % 9;
        }
        rows[{b.row, dir}].push_back(&b);
      }
      a.inRow.assign(d.buildings.size(), 0);
      int made = 0;
      for (auto& [id, members] : rows)
        if (placeRow(vm, grid, a, members, buildings, cache)) {
          ++made;
          for (const BuildingIn* b : members) a.inRow[size_t(b - d.buildings.data())] = 1;
        }
      LOG_INFO("osm: %d of %zu rows built as city houses", made, rows.size());
    }
    while (a.next < d.buildings.size()) {
      if (a.inRow[a.next]) {
        ++a.next;
        continue;
      }
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
    if (std::getenv("REGS_DEBUG_OSM") && a.next == 0) {
      std::string all;
      for (auto& n : names) all += n + " ";
      LOG_INFO("osm: tree templates: %s", all.c_str());
    }
    gridEditBegin(vm, grid);
    // trees and bushes only (owner's request 2026-11-08: no stones, grass or stumps in the woods); woods mostly trees
    // with bushes between, scrub mostly bushes
    std::vector<std::string> treeNames, bushNames;
    for (const std::string& n : names)
      if (n.rfind("bush", 0) == 0) bushNames.push_back(n);
      else if (n.rfind("tree", 0) == 0 || n == "street-tree") treeNames.push_back(n);
    if (treeNames.empty()) treeNames = bushNames;
    if (bushNames.empty()) bushNames = treeNames;
    // woods: big crowns only (tree6 is the bare 'scrub tree', tree5 the small spruce, the street tree a small one); scrub and
    // rows keep them
    std::vector<std::string> woodNames;
    for (const std::string& n : treeNames)
      if (n != "tree6" && n != "tree5" && n != "street-tree") woodNames.push_back(n);  // big crowns
    if (woodNames.empty()) woodNames = treeNames;
    std::vector<std::string> orchardNames;  // round broadleaf crowns for fruit trees
    for (const std::string& n : treeNames)
      if (n == "tree1" || n == "tree2") orchardNames.push_back(n);
    if (orchardNames.empty()) orchardNames = treeNames;
    while (!treeNames.empty() && a.next < d.trees.size()) {
      const TreeIn& t = d.trees[a.next++];
      const int x = t.first, z = t.second;
      size_t i = size_t(z) * size_t(W) + size_t(x);
      if (x < 1 || z < 1 || x >= W - 1 || z >= L - 1 || a.used[i]) continue;
      uint32_t h = hash32((uint64_t(x) << 32) | uint32_t(z));
      // owner 2026-11-12: woods really dense. Woods: a big tree in every cell, a second one in 6 of 10, a bush in 2 of 10;
      // scrub: a bush in every cell and a tree in 40 %; each turned and shifted freely (no cell grid to see)
      const int bushPercent = t.kind == 0 ? 0 : t.kind == 1 ? 100 : t.kind == 2 ? 15 : 0;
      const int secondPercent = t.kind == 0 ? 85 : t.kind == 1 ? 60 : 0;
      const float half = g.cellSize * 0.45f;
      auto plant = [&](const std::string& name, uint32_t hh) {
        int made = addItem(vm, grid, a, name, x, meanCorner(g, x, z), z, int((hh >> 8) & 3));
        if (made < 0) return false;
        GridData::Item& it = g.items[size_t(made)];
        if (t.kind <= 1) {
          it.yaw = (float((hh >> 3) % 1000) / 1000.0f - 0.5f) * 1.5707963f;
          it.offX = (float((hh >> 13) % 1000) / 1000.0f * 2.0f - 1.0f) * half;
          it.offZ = (float((hh >> 23) % 512) / 512.0f * 2.0f - 1.0f) * half;
        }
        ++a.treesPlaced;
        return true;
      };
      const bool bushFirst = int((h >> 12) % 100) < bushPercent;
      const auto& trees = t.kind == 0 ? woodNames : t.kind == 4 ? orchardNames : treeNames;
      const auto& pool = bushFirst ? bushNames : trees;
      if (plant(pool[h % pool.size()], h)) {
        a.used[i] = 4;
        uint32_t h2 = hash32(uint64_t(h) * 2654435761u + 17);
        if (int(h2 % 100) < secondPercent) {
          // woods: mostly a second tree (the canopy closes), else a bush as undergrowth
          const auto& pool2 = bushFirst ? trees : (t.kind == 0 && (h2 >> 20) % 100 < 70) ? trees : bushNames;
          plant(pool2[(h2 >> 7) % pool2.size()], h2);
        }
      }
      if (now() - t0 > budget) break;
    }
    gridEditEnd(vm, grid);
    if (!treeNames.empty() && a.next < d.trees.size()) {
      setP(8, float(a.next) / float(d.trees.size()));
      return false;
    }
    a.stage = 5;
  }
  // done: the new objects on the timeline, their events, the overlay picture
  for (int i : a.made)
    if (size_t(i) < g.items.size() && !g.items[size_t(i)].removed) timelineRecordCreate(vm, a.timeline, grid, uint32_t(i));
  if (a.timeline) timelineEndGroup(vm, a.timeline);
  if (std::getenv("REGS_DEBUG_OSM")) {
    int w;
    float up = groundAbovePavement(g, g.curves, w);
    LOG_INFO("osm: ground above pavement at the end: %.2f (curve %d)", up, w);
  }
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
  LOG_INFO("osm: applied: %d curves, %d road pieces, %d blocks without, %d buildings, %d trees", a.curvesMade, a.roadsOk,
           a.roadsFailed, a.buildingsPlaced, a.treesPlaced);
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
