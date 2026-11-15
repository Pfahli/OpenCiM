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
#include "engine/paint_edges.h"
#include "engine/platform.h"
#include "engine/timeline.h"
#include "engine/tool_tracks.h"
#include "engine/tools.h"
#include "render/image.h"
#include "render/renderer2d.h"
#include "vfs/vfs.h"

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
  std::vector<RoadIn> restParts;
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
      // which of b's points lie beside a (before a moves)
      std::vector<char> by(b.pts.size(), 0);
      for (size_t i = 0; i < b.pts.size(); ++i) by[i] = beside(b.pts[i], a, d, f) && d < maxGap;
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
      // b's stretches not beside a stay, joined to a's new line at their ends (owner 2026-11-14: a track of OSM's that
      // runs beside another only part of its way left a gap when dropped whole)
      for (size_t i = 0; i < b.pts.size();) {
        if (by[i]) {
          ++i;
          continue;
        }
        size_t j = i;
        while (j < b.pts.size() && !by[j]) ++j;
        RoadIn rest = b;
        rest.pts.clear();
        if (i > 0) beside(b.pts[i - 1], a, d, f), rest.pts.push_back(f);
        for (size_t k = i; k < j; ++k) rest.pts.push_back(b.pts[k]);
        if (j < b.pts.size()) beside(b.pts[j], a, d, f), rest.pts.push_back(f);
        if (rest.pts.size() >= 2) restParts.push_back(std::move(rest));
        i = j;
      }
      for (size_t i = 0; i < b.pts.size(); ++i) {
        if (!by[i]) continue;
        beside(b.pts[i], a, d, f);
        moved[key(b.pts[i])] = f;
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
  for (RoadIn& r : restParts) roads.push_back(std::move(r));
  if (merged) LOG_INFO("osm: %d railway tracks merged into double tracks (%zu stretches of them kept)", merged, restParts.size());
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
// an area to furnish (owner 2026-11-14: squares with fountains, car parks with rows of bays): its ring (cells)
struct AreaIn {
  std::vector<std::pair<double, double>> pts;
  int kind = 0;  // 0 square, 1 car park, 2 market place
};
// a street furniture point from OSM (cells): 0 fountain, 1 bench, 2 litter bin, 3 bicycle stand, 4 telephone, 5 post box,
// 6 toilets, 7 street lamp, 8 memorial, 9 monument, 10 artwork, 11 kiosk, 12 clock, 13 playground
struct PointIn {
  double x = 0, z = 0;
  int kind = 0;
};
struct ImportData {
  int W = 0, L = 0, flags = 0;
  double cellM = 2.5;            // metres per cell
  std::vector<AreaIn> areas;
  std::vector<PointIn> points;
  // painted areas with exact edges (paint_edges.h, 2026-11-14): car parks in asphalt (0), squares and market places in
  // paving stones (2); rings in corner coordinates (= cells), several even-odd
  // (type | 0x100: buildings may stand on it, a pedestrian street's band)
  std::vector<std::pair<std::vector<PaintRing>, int>> paintAreas;
  std::vector<std::vector<std::pair<double, double>>> tramLines;  // railway=tram (cells): the streets they run on get tracks
  std::vector<int16_t> heights;  // per corner, engine units (empty: terrain unchanged)
  std::vector<uint8_t> water;    // per cell
  std::vector<uint8_t> paving;   // per cell: 0 none, else the pavement paint type + 1 (1 asphalt, 3 paving stones; 4 a
                                 // pedestrian street's paving stones, buildings allowed)
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
           ";way[\"railway\"~\"^(rail|light_rail)$\"]" + b + ";node[\"railway\"~\"^(station|halt)$\"]" + b +
           ";way[\"highway\"=\"service\"]" + b + ";way[\"railway\"=\"tram\"]" + b +
           ";node[\"amenity\"~\"^(fountain|bench|waste_basket|bicycle_parking|telephone|post_box|toilets|clock)$\"]" + b +
           ";node[\"highway\"=\"street_lamp\"]" + b + ";node[\"historic\"~\"^(memorial|monument)$\"]" + b +
           ";node[\"tourism\"=\"artwork\"]" + b + ";node[\"shop\"=\"kiosk\"]" + b + ";nwr[\"leisure\"=\"playground\"]" + b +
           ";way[\"amenity\"~\"^(marketplace|fountain)$\"]" + b + ";";
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
  // lines of paving: the line, its paving value and half width (metres)
  std::vector<std::pair<std::vector<std::pair<double, double>>, std::pair<uint8_t, double>>> pavedLines;
  std::vector<std::pair<std::vector<std::pair<double, double>>, double>> rivers;  // line, width in cells
  const double cellM = fr.unitsPerCell * metresPerUnit(fr.cmy);
  data->cellM = cellM;
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
          if (flags & kStreets) {  // street furniture (owner 2026-11-14: much OSM data still unused)
            static const std::map<std::string, int> kAmenity = {{"fountain", 0}, {"bench", 1}, {"waste_basket", 2},
                {"bicycle_parking", 3}, {"telephone", 4}, {"post_box", 5}, {"toilets", 6}, {"clock", 12}};
            int kind = -1;
            if (auto it = kAmenity.find(tags.str("amenity")); it != kAmenity.end()) kind = it->second;
            else if (tags.str("highway") == "street_lamp") kind = 7;
            else if (tags.str("historic") == "memorial") kind = 8;
            else if (tags.str("historic") == "monument") kind = 9;
            else if (tags.str("tourism") == "artwork") kind = 10;
            else if (tags.str("shop") == "kiosk") kind = 11;
            else if (tags.str("leisure") == "playground") kind = 13;
            if (kind >= 0) {
              PointIn pt;
              pt.x = fr.cellX(lonToMx(el.num("lon"))), pt.z = fr.cellZ(latToMy(el.num("lat"))), pt.kind = kind;
              if (pt.x >= 1 && pt.z >= 1 && pt.x < W - 1 && pt.z < L - 1) data->points.push_back(pt);
              continue;
            }
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
        std::vector<PaintRing> rings;  // the way's ring or the relation's members (for exact paint)
        bool closed = false;
        if (type == "way") {
          if (const Json* g = el.get("geometry")) pts = P(*g);
          closed = pts.size() > 3 && pts.front() == pts.back();
          addRing(ringEdges, pts, true);
          rings.push_back(pts);
        } else if (type == "relation") {
          if (const Json* mem = el.get("members"); mem && mem->t == Json::Arr)
            for (const Json& m : mem->a)
              if (const Json* g = m.get("geometry")) {
                auto mp = P(*g);
                addRing(ringEdges, mp, false);
                rings.push_back(mp);
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
          // owner 2026-11-14: car parks in asphalt with rows of bays, squares and market places furnished
          const std::string am = tags.str("amenity");
          auto centroidPoint = [&](int kind) {
            double cx = 0, cz = 0;
            for (auto& q : pts) cx += q.first, cz += q.second;
            PointIn pt;
            pt.x = cx / double(pts.size()), pt.z = cz / double(pts.size()), pt.kind = kind;
            if (pt.x >= 1 && pt.z >= 1 && pt.x < W - 1 && pt.z < L - 1) data->points.push_back(pt);
          };
          if (closed && square) {
            data->paintAreas.push_back({rings, 2});
            if (type == "way") data->areas.push_back({pts, 0});
          } else if (closed && am == "marketplace") {
            data->paintAreas.push_back({rings, 2});
            if (type == "way") data->areas.push_back({pts, 2});
          } else if (closed && am == "parking" && tags.str("parking") != "underground" && tags.str("parking") != "multi-storey" &&
                     tags.str("parking") != "rooftop" && tags.str("parking") != "lane" && tags.str("parking") != "street_side" &&
                     tags.str("parking") != "on_street") {  // (parking lanes lie on the streets' shoulders: no strips beside them)
            data->paintAreas.push_back({rings, 0});
            if (type == "way") data->areas.push_back({pts, 1});
          } else if (hw == "service" && type == "way" && closed && tags.str("area") == "yes") {
            // service areas (yards, forecourts) in asphalt. Bands of paint along pedestrian streets and service roads
            // are left out (owner 2026-11-14: painted cells along a slanted line make a saw-tooth strip; pedestrian
            // streets are footpath curves of their own)
            data->paintAreas.push_back({rings, 0});
          } else if (hw == "pedestrian" && type == "way" && !closed) {
            // a pedestrian street: a band of paving stones 6 m wide with exact edges (owner 2026-11-14), a rectangle per
            // leg and an octagon at each bend; buildings may stand on it
            const double hwid = 3.0 / cellM;
            for (size_t k = 0; k + 1 < pts.size(); ++k) {
              auto a = pts[k], b2 = pts[k + 1];
              double dx = b2.first - a.first, dz = b2.second - a.second, l = std::hypot(dx, dz);
              if (l < 1e-6) continue;
              double nx = -dz / l * hwid, nz = dx / l * hwid;
              data->paintAreas.push_back({{{{a.first + nx, a.second + nz}, {b2.first + nx, b2.second + nz},
                                            {b2.first - nx, b2.second - nz}, {a.first - nx, a.second - nz}}},
                                          2 | 0x100});
            }
            for (size_t k = 1; k + 1 < pts.size(); ++k) {
              PaintRing oct;
              for (int j = 0; j < 8; ++j)
                oct.push_back({pts[k].first + hwid * std::cos(j * kPi / 4), pts[k].second + hwid * std::sin(j * kPi / 4)});
              data->paintAreas.push_back({{oct}, 2 | 0x100});
            }
          } else if (am == "fountain") {
            centroidPoint(0);
          } else if (tags.str("leisure") == "playground") {
            centroidPoint(13);
          }
          if (square || am == "parking" || am == "marketplace" || am == "fountain" || hw == "service" ||
              tags.str("leisure") == "playground" || !tags.str("area:highway").empty())
            continue;
        }
        // tram lines (owner 2026-11-14): the streets they run along get the curve's street tram tracks
        if ((flags & kStreets) && type == "way" && tags.str("railway") == "tram") {
          if (tags.str("tunnel").empty() && tags.num("layer", 0) >= 0) data->tramLines.push_back(pts);
          continue;
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
        } else if ((ww == "river" || ww == "canal") && tags.str("tunnel").empty() && tags.str("covered") != "yes" &&
                   tags.num("layer", 0) >= 0) {
          // (culverts and covered stretches run under the town: no open water there; owner 2026-11-14, the Saalbach
          // under Bruchsal's market place)
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
  // squares, market places and car parks are dry (a stream culverted under a market place; owner 2026-11-14)
  if (flags & kWater)
    for (auto& [rings, type] : data->paintAreas)
      if (!(type & 0x100))
        for (const PaintAreaCell& c : paintAreaCells(W, L, rings))
          if (c.full) water[size_t(c.z) * size_t(W) + size_t(c.x)] = 0;
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
    // a pedestrian street: 6 m of paving stones (4: buildings may stand on it); service roads asphalt
    for (auto& [line, how] : pavedLines) {
      const uint8_t value = how.first;
      const double half = how.second / cellM;
      for (size_t k = 0; k + 1 < line.size(); ++k) {
        auto a = line[k], b2 = line[k + 1];
        int x0 = int(std::floor(std::min(a.first, b2.first) - half)), x1 = int(std::ceil(std::max(a.first, b2.first) + half));
        int z0 = int(std::floor(std::min(a.second, b2.second) - half)), z1 = int(std::ceil(std::max(a.second, b2.second) + half));
        for (int z = std::max(0, z0); z <= std::min(L - 1, z1); ++z)
          for (int x = std::max(0, x0); x <= std::min(W - 1, x1); ++x) {
            size_t i = size_t(z) * size_t(W) + size_t(x);
            if (!water[i] && (!data->paving[i] || (value == 1 && data->paving[i] == 4)) && segDist(x + 0.5, z + 0.5, a, b2) <= half)
              data->paving[i] = value;
          }
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
  LOG_INFO("osm: import data: %zu roads, %zu buildings, %zu trees, %zu areas, %zu furniture points, terrain %d, satellite %dx%d",
           data->roads.size(), data->buildings.size(), data->trees.size(), data->areas.size(), data->points.size(),
           int(!data->heights.empty()), data->satW, data->satH);
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
  std::vector<std::pair<double, double>> yardSeeds;  // free cells behind the rows of houses (cells)
  std::vector<std::vector<std::pair<double, double>>> yardBands;  // a band of pavement behind each row (corner coordinates)
  int stationsPlaced = 0;
  std::vector<std::array<double, 4>> stationFrames;  // placed stations: centre (cells) and front  // REGS_OSM_GRID: the streets on the block grid (before the curve tools)
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

Plan plan(const std::vector<RoadIn>& roads, int W, int L, float cs, const std::vector<uint8_t>& water,
          const std::vector<int16_t>& height) {
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
    // the margin left for the edge pieces; tracks have none and run to the edge (owner 2026-11-14: they ended short of it)
    const double m = c.kind == 6 ? 1.0 : c.kind == 3 ? 2.0 : c.kind == 0 ? 8.0 : 14.0;
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
              double t0 = double(runStart) / n - m, t1 = (w ? t : double(k - 1) / n) + m;
              // a cut deeper than the banks (a walled stream): on until the ground is back near the banks' level
              if (!height.empty()) {
                auto H = [&](double tt) {
                  tt = std::clamp(tt, 0.0, 1.0);
                  int cx = std::clamp(int(std::lround(p.first + (q.first - p.first) * tt)), 0, W - 1);
                  int cz = std::clamp(int(std::lround(p.second + (q.second - p.second) * tt)), 0, L - 1);
                  return double(height[size_t(cz) * size_t(W) + size_t(cx)]);
                };
                const double bank = std::min(H(0), H(1)) - 0.8 * 256.0 / cs;  // 0.8 units under the lower bank
                const double step = 0.5 / len;
                while (t0 > 0 && H(t0) < bank) t0 -= step;
                while (t1 < 1 && H(t1) < bank) t1 += step;
                t0 -= m * 0.5, t1 += m * 0.5;
              }
              iv.push_back({t0, t1});
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

// tracks that end in the open (owner 2026-11-14: rails that just end make no sense): spurs up to 120 cells from a
// branch to a free end and loose pieces up to 400 cells are dropped, again until none is left; ends at the map edge stay
void pruneTracks(Plan& P, int W, int L, double cs) {
  auto atEdge = [&](const Node& n) {
    return std::fabs(n.x) > (W * 0.5 - 4.0) * cs || std::fabs(n.z) > (L * 0.5 - 4.0) * cs;
  };
  int dropped = 0;
  for (int iter = 0; iter < 400; ++iter) {
    std::vector<std::vector<int>> at(P.nodes.size());
    for (size_t si = 0; si < P.segs.size(); ++si) {
      const Seg& sg = P.segs[si];
      if (!sg.alive || !curveIsTrack(sg.kind)) continue;
      at[size_t(sg.a)].push_back(int(si)), at[size_t(sg.b)].push_back(int(si));
    }
    bool changed = false;
    for (size_t n = 0; n < P.nodes.size(); ++n) {
      if (at[n].size() != 1 || atEdge(P.nodes[n])) continue;
      std::vector<int> path;
      double len = 0;
      int cur = int(n), seg = at[n][0];
      std::set<int> seen;
      while (true) {
        if (seen.count(seg)) break;
        seen.insert(seg);
        path.push_back(seg);
        const Seg& sg = P.segs[size_t(seg)];
        int nx = sg.a == cur ? sg.b : sg.a;
        len += std::hypot(P.nodes[size_t(nx)].x - P.nodes[size_t(cur)].x, P.nodes[size_t(nx)].z - P.nodes[size_t(cur)].z) / cs;
        cur = nx;
        if (at[size_t(cur)].size() != 2 || atEdge(P.nodes[size_t(cur)])) break;
        seg = at[size_t(cur)][0] == seg ? at[size_t(cur)][1] : at[size_t(cur)][0];
      }
      const size_t endDeg = at[size_t(cur)].size();
      const bool spur = endDeg >= 3 && len < 120.0;
      const bool loose = endDeg == 1 && !atEdge(P.nodes[size_t(cur)]) && len < 400.0;
      if (!spur && !loose) continue;
      for (int si : path) P.segs[size_t(si)].alive = false;
      dropped += int(path.size());
      changed = true;
      break;  // the node lists are stale now
    }
    if (!changed) break;
  }
  if (dropped) LOG_INFO("osm: %d track legs ending in the open dropped", dropped);
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
    if (std::getenv("REGS_DEBUG_OSM")) LOG_INFO("osm: roundabout at world %.1f, %.1f, radius %.1f, %zu arms", cx, cz, R, arms.size());
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
    float r = curveProfile(c.kind).halfWidth + (c.bridge ? 4.0f : 1.5f);  // bridges: room for their parapets and ramps
    for (const CurveSample& s : sampleCurve(c, 1.5f)) {
      int x0 = int(std::floor((s.p.x - r) / cell + g.width * 0.5f)), x1 = int(std::floor((s.p.x + r) / cell + g.width * 0.5f));
      int z0 = int(std::floor((s.p.z - r) / cell + g.length * 0.5f)), z1 = int(std::floor((s.p.z + r) / cell + g.length * 0.5f));
      for (int z = std::max(0, z0); z <= std::min(g.length - 1, z1); ++z)
        for (int x = std::max(0, x0); x <= std::min(g.width - 1, x1); ++x) {
          float wx = (float(x) + 0.5f - g.width * 0.5f) * cell - s.p.x, wz = (float(z) + 0.5f - g.length * 0.5f) * cell - s.p.z;
          if (wx * wx + wz * wz <= (r + 1.0f) * (r + 1.0f)) {
            uint8_t& u = used[size_t(z) * size_t(g.width) + size_t(x)];
            u = curveIsTrack(c.kind) ? (u == 2 ? 2 : 7) : 2;  // 7: a track's (no street furniture there)
          }
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
      // signs in the building lists (the grocery's signs among the commercial buildings) are no buildings: in a small
      // footprint they stood in rows in the yards (owner 2026-11-14)
      if (name.find("sign") != std::string::npos) continue;
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
    float cw = 0, s = 0;     // the kerb's distance from the centre, the arc length (cells)
    int ci = -1, kind = 0;   // the curve and its kind
  };
  std::vector<P> pts;
  std::map<std::pair<int, int>, std::vector<uint32_t>> bucket;  // 8 × 8 cells
  void build(const GridData& g) {
    pts.clear(), bucket.clear();
    const float cs = g.cellSize;
    for (size_t ci = 0; ci < g.curves.size(); ++ci) {
      const CurveRoad& c = g.curves[ci];
      if (c.kind == 3 || curveIsTrack(c.kind)) continue;  // footpaths and tracks: houses face the streets
      const float hw = curveProfile(c.kind).halfWidth / cs;
      for (const CurveSample& sm : sampleCurve(c, 2.0f)) {
        float l = std::hypot(sm.dir.x, sm.dir.z);
        if (l < 1e-4f) continue;
        P q{sm.p.x / cs + g.width * 0.5f, sm.p.z / cs + g.length * 0.5f, sm.dir.x / l, sm.dir.z / l, hw};
        q.cw = curveProfile(c.kind).carriageway / cs, q.s = sm.s / cs, q.ci = int(ci), q.kind = c.kind;
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

// a template's footprint (cells)
bool templateSize(VM& vm, GridObj* grid, const std::string& name, int& w, int& l) {
  auto it = grid->templates.find(name);
  if (it == grid->templates.end()) return false;
  Object* o = it->second.get();
  auto f = [&](const char16_t* key) {
    auto fv = o->fieldValues.find(key);
    return fv == o->fieldValues.end() || fv->second.isNone() ? 1 : std::max(1, int(vm.toInt(fv->second)));
  };
  w = f(u"width"), l = f(u"length");
  return true;
}

// the turn of an object whose model front (−Z) faces (dx, dz) (cells): the quarter turn and the free turn left over
void faceTurn(double dx, double dz, int& orient, float& yaw) {
  const double q = kPi / 2, th = std::atan2(dz, dx);
  const double k = std::round(th / q), rem = th - k * q;
  static const int side[4] = {1, 2, 3, 0};  // east, south, west, north
  orient = frontOrient(side[((int(k) % 4) + 4) % 4]);
  yaw = float(-rem);
}
// the model front for an object whose model +X (a lamp's arm) should point to (dx, dz)
inline void armFront(double dx, double dz, double& fx, double& fz) { fx = dz, fz = -dx; }

constexpr int kGroundY = -0x7fffffff;
std::map<std::string, int> g_placedCount;  // REGS_DEBUG_OSM: what the furnishing placed
// an object centred at (px, pz) (cells), its model front to (fx, fz): the quarter turn, the free turn and the shift onto
// the point. Every cell its turned footprint covers must pass ok(used value); they are marked `mark` (road cells keep 2).
// y: the height (engine units), kGroundY the ground. Returns the item or -1
template <class F>
int placeAt(VM& vm, GridObj* grid, Apply& a, const std::string& name, double px, double pz, double fx, double fz, F ok,
            uint8_t mark, int y = kGroundY) {
  GridData& g = *grid->data;
  const int W = g.width, L = g.length;
  int w, l;
  if (!templateSize(vm, grid, name, w, l)) return -1;
  int orient;
  float yaw;
  faceTurn(fx, fz, orient, yaw);
  const int sx = (orient & 1) ? l : w, sz = (orient & 1) ? w : l;
  const double rem = -yaw, Ux = std::cos(rem), Uz = std::sin(rem), Vx = -Uz, Vz = Ux;
  const int x = int(std::lround(px - sx * 0.5)), z = int(std::lround(pz - sz * 0.5));
  if (x < 1 || z < 1 || x + sx >= W || z + sz >= L) return -1;
  std::vector<size_t> cells;
  const double hx = std::max(0.35, sx * 0.5 - 0.15), hz = std::max(0.35, sz * 0.5 - 0.15), r = std::hypot(hx, hz);
  for (int zz = int(std::floor(pz - r)); zz <= int(std::ceil(pz + r)); ++zz)
    for (int xx = int(std::floor(px - r)); xx <= int(std::ceil(px + r)); ++xx) {
      double dx = xx + 0.5 - px, dz = zz + 0.5 - pz;
      if (std::fabs(dx * Ux + dz * Uz) > hx || std::fabs(dx * Vx + dz * Vz) > hz) continue;
      if (xx < 1 || zz < 1 || xx >= W - 1 || zz >= L - 1) return -1;
      size_t ci = size_t(zz) * size_t(W) + size_t(xx);
      if (!ok(a.used[ci])) return -1;
      cells.push_back(ci);
    }
  if (cells.empty()) {
    int cx = int(std::floor(px)), cz = int(std::floor(pz));
    size_t ci = size_t(cz) * size_t(W) + size_t(cx);
    if (!ok(a.used[ci])) return -1;
    cells.push_back(ci);
  }
  if (y == kGroundY) y = sx * sz > 1 ? maxCorner(g, x, z, sx, sz) : meanCorner(g, x, z);
  int made = addItem(vm, grid, a, name, x, y, z, orient);
  if (made < 0) return -1;
  ++g_placedCount[name];
  GridData::Item& it = g.items[size_t(made)];
  const float half = g.cellSize * 0.5f;
  it.yaw = std::fabs(yaw) < 0.5f * float(kPi) / 180.0f ? 0.0f : yaw;
  it.offX = std::clamp(float((px - (x + sx * 0.5)) * g.cellSize), -half, half);
  it.offZ = std::clamp(float((pz - (z + sz * 0.5)) * g.cellSize), -half, half);
  if (mark)
    for (size_t ci : cells)
      if (a.used[ci] != 2 && a.used[ci] != 7) a.used[ci] = mark;
  return made;
}

// ---- the blocks of row houses (owner 2026-11-14: an autogen system for row houses based on blocks of houses) ----
// Buildings wall to wall form a block. Each building belongs to the side of the block along the street in front of it
// (the nearest curve and its side); a side is cut where the street bends more than 12°. Each side becomes a straight
// row on the street's building line (the median of its buildings' street walls, at least clear of the pavement), as
// deep as its buildings are; two sides meeting at an outer corner of the block (60–120° apart, their building lines
// crossing near both ends) get a corner house there that both rows run up to. Then each row is filled edge to edge with
// city houses of one family (the block's), leftover width taken up by wider houses. Behind the rows the yards are
// furnished later (decorateYards).
struct RowPlan {
  double ox = 0, oz = 0, ux = 1, uz = 0, vx = 0, vz = 1;  // a point on the street, along it, across it to the houses (cells)
  double t[2] = {0, 0};                                  // the row's ends along u
  double front = 0, depth = 0, back = 0;                 // the building line and the depth (from the street's centre)
  bool corner[2] = {false, false};
  std::vector<const BuildingIn*> members;
};

int placeBlock(VM& vm, GridObj* grid, Apply& a, const std::vector<const BuildingIn*>& block, const Value& buildingsList,
               std::map<std::string, std::vector<Candidate>>& cache) {
  // 1. the block's sides
  struct M {
    const BuildingIn* b;
    const StreetIndex::P* st;
  };
  std::map<std::pair<int, int>, std::vector<M>> sides;
  for (const BuildingIn* b : block) {
    double cx = 0, cz = 0;
    for (auto& q : b->pts) cx += q.first, cz += q.second;
    cx /= double(b->pts.size()), cz /= double(b->pts.size());
    const StreetIndex::P* st = g_streets.nearest(cx, cz, 12.0);
    if (!st) continue;
    int side = st->tx * (cz - st->z) - st->tz * (cx - st->x) > 0 ? 1 : 0;
    sides[{st->ci, side}].push_back({b, st});
  }
  std::vector<RowPlan> rows;
  const double bend = std::cos(12.0 * kPi / 180.0);
  for (auto& [key, ms] : sides) {
    std::sort(ms.begin(), ms.end(), [](const M& p, const M& q) { return p.st->s < q.st->s; });
    for (size_t i = 0; i < ms.size();) {
      size_t j = i + 1;
      while (j < ms.size() && ms[j].st->tx * ms[i].st->tx + ms[j].st->tz * ms[i].st->tz > bend && ms[j].st->s - ms[j - 1].st->s < 16.0) ++j;
      RowPlan r;
      double ux = 0, uz = 0, ox = 0, oz = 0, mx = 0, mz = 0, n = 0;
      for (size_t k = i; k < j; ++k) {
        ux += ms[k].st->tx, uz += ms[k].st->tz, ox += ms[k].st->x, oz += ms[k].st->z;
        for (auto& q : ms[k].b->pts) mx += q.first, mz += q.second, n += 1;
      }
      double ul = std::hypot(ux, uz);
      if (ul < 1e-6 || n == 0) {
        i = j;
        continue;
      }
      r.ux = ux / ul, r.uz = uz / ul, r.ox = ox / double(j - i), r.oz = oz / double(j - i);
      r.vx = -r.uz, r.vz = r.ux;
      if ((mx / n - r.ox) * r.vx + (mz / n - r.oz) * r.vz < 0) r.vx = -r.vx, r.vz = -r.vz;
      std::vector<double> fronts, depths;
      r.t[0] = 1e30, r.t[1] = -1e30;
      for (size_t k = i; k < j; ++k) {
        double fm = 1e30, bm = -1e30;
        for (auto& q : ms[k].b->pts) {
          double dx = q.first - r.ox, dz = q.second - r.oz, t = dx * r.ux + dz * r.uz, w = dx * r.vx + dz * r.vz;
          r.t[0] = std::min(r.t[0], t), r.t[1] = std::max(r.t[1], t);
          fm = std::min(fm, w), bm = std::max(bm, w);
        }
        fronts.push_back(fm), depths.push_back(bm - fm);
        r.members.push_back(ms[k].b);
      }
      std::sort(fronts.begin(), fronts.end());
      std::sort(depths.begin(), depths.end());
      r.front = std::max(fronts[fronts.size() / 2], double(ms[i].st->hw) + 0.9);
      r.depth = std::clamp(depths[depths.size() / 2], 4.0, 14.0);
      if (r.t[1] - r.t[0] >= 3.0) rows.push_back(r);
      i = j;
    }
  }
  if (rows.empty()) return 0;
  // 2. the houses: one family for the block (the base game's city houses or a package's row houses, deep enough)
  auto it = cache.find("blockhouses");
  if (it == cache.end()) it = cache.emplace("blockhouses", categoryTemplates(vm, grid, buildingsList, "blockhouses")).first;
  // the original maps (research 2026-11-13): city houses have their street on model −Z; the corner houses 01, 02, 05,
  // 06 on −Z and +X, 03, 04 and small-townbuilding04 on −Z and −X
  std::vector<const Candidate*> cornersPlusX, cornersMinusX;
  std::map<std::string, std::vector<const Candidate*>> families;
  auto family = [](const std::string& nm) {
    if (nm.rfind("cityhouse", 0) == 0 || nm.rfind("small-townbuilding", 0) == 0) return std::string("base");
    std::string f = nm;
    while (!f.empty() && (std::isdigit((unsigned char)f.back()) || f.back() == '-')) f.pop_back();
    return f;
  };
  for (const Candidate& c : it->second) {
    const std::string& nm = c.name;
    if (nm == "cornerhouse03" || nm == "cornerhouse04" || nm == "small-townbuilding04") cornersMinusX.push_back(&c);
    else if (nm.rfind("cornerhouse", 0) == 0) cornersPlusX.push_back(&c);
    else if (nm.rfind("cityhouse", 0) == 0 || nm.rfind("small-townbuilding", 0) == 0) families["base"].push_back(&c);
    else if (nm.find("diagonal") == std::string::npos && !(nm.size() > 2 && nm.compare(nm.size() - 2, 2, "-b") == 0) && c.w <= 7 &&
             c.l >= 4 && c.l <= 12)
      families[family(nm)].push_back(&c);
  }
  std::vector<double> ds;
  for (const RowPlan& r : rows) ds.push_back(r.depth);
  std::sort(ds.begin(), ds.end());
  const double blockDepth = ds[ds.size() / 2];
  std::vector<std::string> keys;
  for (auto& [k, v] : families) {
    bool fits = false;
    for (const Candidate* c : v) fits = fits || c->l <= blockDepth + 2.0;
    if (fits) keys.push_back(k);
  }
  if (keys.empty())
    for (auto& [k, v] : families) keys.push_back(k);
  if (keys.empty()) return 0;
  const uint32_t h = hash32(block.front()->id * 2654435761ull + 5);
  // the base game's city houses for half the blocks (they are made to stand in rows), a package's family otherwise
  std::string fam = keys[h % keys.size()];
  if (families.count("base") && std::find(keys.begin(), keys.end(), "base") != keys.end() && (h >> 12) % 2 == 0) fam = "base";
  const std::vector<const Candidate*>& houses = families[fam];
  auto free0 = [](uint8_t u) { return u == 0; };
  // 3. the corners
  for (size_t i = 0; i < rows.size(); ++i)
    for (size_t j = 0; j < rows.size(); ++j) {
      if (i == j) continue;
      RowPlan &A = rows[i], &B = rows[j];
      if (std::fabs(A.ux * B.ux + A.uz * B.uz) > 0.5) continue;
      if (A.t[1] - A.t[0] < B.t[1] - B.t[0]) continue;  // A: the longer side, the corner house's front on it
      double ax = A.ox + A.vx * A.front, az = A.oz + A.vz * A.front, bx = B.ox + B.vx * B.front, bz = B.oz + B.vz * B.front;
      double den = A.ux * B.uz - A.uz * B.ux;
      if (std::fabs(den) < 1e-6) continue;
      double tA = ((bx - ax) * B.uz - (bz - az) * B.ux) / den;
      double cx = ax + A.ux * tA, cz = az + A.uz * tA;
      double tB = (cx - B.ox) * B.ux + (cz - B.oz) * B.uz;
      int eA = std::fabs(tA - A.t[0]) < std::fabs(tA - A.t[1]) ? 0 : 1, eB = std::fabs(tB - B.t[0]) < std::fabs(tB - B.t[1]) ? 0 : 1;
      if (A.corner[eA] || B.corner[eB]) continue;
      if (std::fabs(tA - A.t[eA]) > B.depth + 4.0 || std::fabs(tB - B.t[eB]) > A.depth + 4.0) continue;
      // an outer corner: each row's houses on the other's house side
      double amx = A.ox + A.ux * (A.t[0] + A.t[1]) * 0.5 + A.vx * (A.front + A.depth * 0.5);
      double amz = A.oz + A.uz * (A.t[0] + A.t[1]) * 0.5 + A.vz * (A.front + A.depth * 0.5);
      double bmx = B.ox + B.ux * (B.t[0] + B.t[1]) * 0.5 + B.vx * (B.front + B.depth * 0.5);
      double bmz = B.oz + B.uz * (B.t[0] + B.t[1]) * 0.5 + B.vz * (B.front + B.depth * 0.5);
      if ((amx - cx) * B.vx + (amz - cz) * B.vz <= 0 || (bmx - cx) * A.vx + (bmz - cz) * A.vz <= 0) continue;
      A.corner[eA] = B.corner[eB] = true;
      const double dA = eA ? -1.0 : 1.0, dB = eB ? -1.0 : 1.0;  // into each row from the corner
      const double fx = -A.vx, fz = -A.vz, plusX = (-fz) * (-B.vx) + fx * (-B.vz);
      const auto& corners = plusX > 0 ? cornersPlusX : cornersMinusX;
      bool made = false;
      if (!corners.empty()) {
        const Candidate& c = *corners[(h >> (3 + (i % 8))) % corners.size()];
        double px = cx + A.ux * dA * c.w * 0.5 + A.vx * c.l * 0.5, pz = cz + A.uz * dA * c.w * 0.5 + A.vz * c.l * 0.5;
        if (placeAt(vm, grid, a, c.name, px, pz, fx, fz, free0, 3) >= 0) {
          ++a.buildingsPlaced;
          A.t[eA] = tA + dA * c.w, B.t[eB] = tB + dB * c.l;
          made = true;
        }
      }
      if (!made) A.t[eA] = tA, B.t[eB] = tB + dB * A.depth;  // A runs up to the corner, B up to A's back
    }
  // 4. the rows, edge to edge
  int placed = 0;
  int minW = 1 << 30;
  for (const Candidate* c : houses) minW = std::min(minW, c->w);
  for (size_t ri = 0; ri < rows.size(); ++ri) {
    RowPlan& r = rows[ri];
    std::vector<const Candidate*> fit;
    for (const Candidate* c : houses)
      if (c->l <= r.depth + 2.0) fit.push_back(c);
    if (fit.empty()) fit = houses;
    int fitMin = 1 << 30;
    for (const Candidate* c : fit) fitMin = std::min(fitMin, c->w);
    const double len = r.t[1] - r.t[0];
    std::vector<const Candidate*> run;
    double used = 0;
    const uint32_t hr = hash32(uint64_t(h) * 31 + ri);
    for (int k = 0; used + fitMin <= len + 0.3 && k < 96; ++k) {
      const Candidate* pick = nullptr;
      for (int tries = 0; tries < 8 && !pick; ++tries) {
        const Candidate* c = fit[hash32(uint64_t(hr) * 7 + uint64_t(k) * 13 + uint64_t(tries)) % fit.size()];
        if (used + c->w <= len + 0.3) pick = c;
      }
      if (!pick)
        for (const Candidate* c : fit)
          if (used + c->w <= len + 0.3 && (!pick || c->w > pick->w)) pick = c;
      if (!pick) break;
      run.push_back(pick);
      used += pick->w;
    }
    // the leftover taken up by wider houses (no gaps in the row)
    for (size_t k = 0; k < run.size() && len - used >= 1.0; ++k) {
      const Candidate* best = nullptr;
      for (const Candidate* c : fit)
        if (c->w > run[k]->w && used - run[k]->w + c->w <= len + 0.3 && (!best || c->w > best->w)) best = c;
      if (best) used += best->w - run[k]->w, run[k] = best;
    }
    double t = r.corner[0] && !r.corner[1] ? r.t[0] : r.corner[1] && !r.corner[0] ? r.t[1] - used : r.t[0] + (len - used) * 0.5;
    int here = 0;
    double back = 0;
    for (const Candidate* c : run) {
      const double tc = t + c->w * 0.5;
      for (double shift : {0.0, 0.5, 1.0}) {
        double across = r.front + c->l * 0.5 + shift;
        double px = r.ox + r.ux * tc + r.vx * across, pz = r.oz + r.uz * tc + r.vz * across;
        if (placeAt(vm, grid, a, c->name, px, pz, -r.vx, -r.vz, free0, 3) >= 0) {
          ++a.buildingsPlaced, ++here;
          back = std::max(back, r.front + c->l + shift);
          break;
        }
      }
      t += c->w;
    }
    if (!here) continue;
    placed += here;
    for (const BuildingIn* b : r.members) a.inRow[size_t(b - a.d->buildings.data())] = 1;
    // the yard's pavement along the houses' backs: a band 2.2 cells deep (painted with exact edges, decorateYards)
    {
      auto P = [&](double t, double w) { return std::pair<double, double>{r.ox + r.ux * t + r.vx * w, r.oz + r.uz * t + r.vz * w}; };
      a.yardBands.push_back({P(r.t[0], back - 0.2), P(r.t[1], back - 0.2), P(r.t[1], back + 2.2), P(r.t[0], back + 2.2)});
    }
    // the yard behind the row: its seeds a cell and a half behind the houses
    for (double ts = r.t[0] + 0.5; ts <= r.t[1] - 0.5; ts += 1.0)
      a.yardSeeds.push_back({r.ox + r.ux * ts + r.vx * (back + 1.5), r.oz + r.uz * ts + r.vz * (back + 1.5)});
  }
  return placed;
}

// ---- templates from the game's own maps (owner 2026-11-14: copy fitting parts of the default maps as templates) ----
// Read from the installed game at the first import (nothing of the game's data goes into the port): in the official
// maps (the base game's cities and the European packages' big maps) every patch of pavement paint with street
// furniture on it is a square template, and every patch of open ground closed mostly by houses with garden things in it
// a yard template; each keeps its cells and its furniture (the props of the level's park, city, residential and market
// lists, the lamps, trees and bushes) relative to its centre. An OSM square or a yard behind a row of houses takes the
// template whose shape covers it best (intersection over union, turned to the area's main axis in 90° steps) and gets
// that furniture where it lands on its own free cells.
struct TplProp {
  std::string name;
  double x, z, fx, fz;  // centre relative to the template's centre (cells), the model front
};
struct TplArea {
  std::set<std::pair<int, int>> cells;  // source cells
  std::map<std::pair<int, int>, uint8_t> paint;  // painted source cells: the pavement type + 1
  double cx = 0, cz = 0, ang = 0;       // centre (cells) and main axis
  std::vector<TplProp> props;
  std::string from;
};
struct TplLibrary {
  bool built = false;
  std::vector<TplArea> squares, yards;
};
TplLibrary g_tpl;

double cellsAxis(const std::vector<std::pair<double, double>>& pts) {
  double ang, ea, eb, cx, cz;
  if (pts.size() < 3 || !minRect(pts, ang, ea, eb, cx, cz)) return 0;
  return ea >= eb ? ang : ang + kPi / 2;
}

void buildTemplates(VM& vm, GridObj* grid, const Value& buildingsList, const Value& trees) {
  if (g_tpl.built) return;
  g_tpl.built = true;
  const double t0 = now();
  std::set<std::string> decor, houses;
  try {
    if (Value* main = vm.findGlobal(u"MAIN"))
      if (MapObj* m = vm.resolve(vm.getMember(vm.getMember(*main, u"$level"), u"$props")).map())
        for (auto& [k, v] : m->entries) {
          std::string cat = utf16ToUtf8(vm.toStr(k));
          if (cat != "park" && cat != "city" && cat != "residential" && cat != "market" && cat != "road") continue;
          if (ArrayObj* arr = vm.resolve(v).array())
            for (const Value& n : arr->items) {
              std::string nm = utf16ToUtf8(vm.toStr(n));
              if (cat == "road" && nm.find("light") == std::string::npos && nm.find("lamp") == std::string::npos) continue;
              decor.insert(nm);
            }
        }
  } catch (const std::exception& e) {
    LOG_WARN("osm: templates: no prop lists (%s)", e.what());
  }
  if (ArrayObj* arr = vm.resolve(trees).array())
    for (const Value& v : arr->items) {
      std::string n = utf16ToUtf8(vm.toStr(v));
      if (n.rfind("tree", 0) == 0 || n.rfind("bush", 0) == 0 || n == "street-tree" || n == "cherry-tree") decor.insert(n);
    }
  if (MapObj* m = vm.resolve(buildingsList).map())
    for (auto& [k, v] : m->entries)
      if (ArrayObj* arr = vm.resolve(v).array())
        for (const Value& n : arr->items) houses.insert(utf16ToUtf8(vm.toStr(n)));
  static const char* kMaps[] = {"metro/maps/berlin.grid",          "metro/maps/amsterdam.grid",
                                "metro/maps/helsinki.grid",        "metro/maps/vienna.grid",
                                "addons/germany/maps/cologne.grid", "addons/germany/maps/leipzig.grid",
                                "addons/munich/maps/munich.grid",   "addons/ulm/maps/ulm.grid",
                                "addons/london/maps/london.grid",   "addons/paris/maps/paris.grid",
                                "addons/stpetersburg/maps/stpetersburg.grid"};
  for (const char* path : kMaps) {
    std::vector<uint8_t> file;
    GridData s;
    std::string err;
    if (!vm.vfs().read(path, file) || !parseGridFile(file, s, &err)) continue;
    const int W = s.width, L = s.length;
    std::string from = path;
    from = from.substr(from.find_last_of('/') + 1);
    // what stands where: 1 furniture, 2 houses, 3 roads and everything else
    std::vector<uint8_t> occ(size_t(W) * size_t(L), 0);
    struct D {
      std::string name;
      double cx, cz, fx, fz;
    };
    std::vector<D> furniture;
    for (const GridData::Item& it : s.items) {
      if (it.parent || it.type >= s.types.size()) continue;
      const std::string& name = s.types[it.type];
      int w = 1, l = 1;
      bool known = templateSize(vm, grid, name, w, l);
      const int sx = (it.orient & 1) ? l : w, sz = (it.orient & 1) ? w : l;
      const uint8_t kind = decor.count(name) ? 1 : houses.count(name) ? 2 : 3;
      for (int zz = it.z; zz < it.z + sz; ++zz)
        for (int xx = it.x; xx < it.x + sx; ++xx)
          if (xx >= 0 && zz >= 0 && xx < W && zz < L) {
            uint8_t& o = occ[size_t(zz) * size_t(W) + size_t(xx)];
            o = std::max(o, kind);
          }
      if (kind == 1 && known) {
        static const double kFront[4][2] = {{0, 1}, {1, 0}, {0, -1}, {-1, 0}};  // orient → the model front (frontOrient's inverse)
        furniture.push_back({name, it.x + sx * 0.5, it.z + sz * 0.5, kFront[it.orient & 3][0], kFront[it.orient & 3][1]});
      }
    }
    std::map<std::pair<int, int>, std::vector<size_t>> at;
    for (size_t i = 0; i < furniture.size(); ++i) at[{int(std::floor(furniture[i].cx)), int(std::floor(furniture[i].cz))}].push_back(i);
    std::vector<uint8_t> seen(size_t(W) * size_t(L), 0);
    int squares = 0, yards = 0;
    for (int pass = 0; pass < 2; ++pass)  // 0 squares (paved), 1 yards (open ground)
      for (int z0 = 1; z0 < L - 1; ++z0)
        for (int x0 = 1; x0 < W - 1; ++x0) {
          auto cellOk = [&](int x, int z) {
            if (x < 1 || z < 1 || x >= W - 1 || z >= L - 1) return false;
            size_t i = size_t(z) * size_t(W) + size_t(x);
            uint16_t f = s.flags[i];
            if (occ[i] >= 2 || (f & 0x2000)) return false;
            return pass == 0 ? (f & 0x8) != 0 : true;  // yards: paved or not (owner 2026-11-14: yards paved)
          };
          size_t i0 = size_t(z0) * size_t(W) + size_t(x0);
          if (seen[i0] == pass + 1 || !cellOk(x0, z0)) continue;
          const size_t cap = pass == 0 ? 2500 : 1200;
          std::vector<std::pair<int, int>> comp{{x0, z0}};
          seen[i0] = uint8_t(pass + 1);
          bool open = false;
          int byHouse = 0, edge = 0;
          for (size_t qi = 0; qi < comp.size(); ++qi) {
            auto [x, z] = comp[qi];
            for (auto [nx, nz] : {std::pair{x - 1, z}, {x + 1, z}, {x, z - 1}, {x, z + 1}}) {
              if (nx < 0 || nz < 0 || nx >= W || nz >= L) {
                open = true;
                continue;
              }
              size_t ni = size_t(nz) * size_t(W) + size_t(nx);
              if (cellOk(nx, nz)) {
                if (seen[ni] == pass + 1) continue;
                seen[ni] = uint8_t(pass + 1);
                if (comp.size() < cap) comp.push_back({nx, nz});
                else open = true;
              } else {
                ++edge;
                byHouse += occ[ni] == 2;
              }
            }
          }
          if (open || comp.size() < (pass == 0 ? 25u : 15u)) continue;
          if (pass == 1 && byHouse * 100 < edge * 45) continue;  // a yard: mostly houses around it
          TplArea t;
          t.from = from;
          std::vector<std::pair<double, double>> pts;
          for (auto& c : comp) {
            t.cells.insert(c);
            if (uint16_t f = s.flags[size_t(c.second) * size_t(W) + size_t(c.first)]; pass == 1 && (f & 0x8)) t.paint[c] = uint8_t((f & 7) + 1);
            t.cx += c.first + 0.5, t.cz += c.second + 0.5;
            pts.push_back({c.first + 0.5, c.second + 0.5});
          }
          t.cx /= double(comp.size()), t.cz /= double(comp.size());
          t.ang = cellsAxis(pts);
          std::set<std::string> kinds;
          for (auto& c : comp) {
            auto it = at.find(c);
            if (it == at.end()) continue;
            for (size_t k : it->second) {
              const D& dd = furniture[k];
              t.props.push_back({dd.name, dd.cx - t.cx, dd.cz - t.cz, dd.fx, dd.fz});
              kinds.insert(dd.name);
            }
          }
          if (t.props.size() < (pass == 0 ? 4u : 3u) || kinds.size() < 2) continue;
          (pass == 0 ? g_tpl.squares : g_tpl.yards).push_back(std::move(t));
          ++(pass == 0 ? squares : yards);
        }
    LOG_INFO("osm: templates from %s: %d squares, %d yards", from.c_str(), squares, yards);
  }
  LOG_INFO("osm: %zu square and %zu yard templates from the game's maps in %.1f s", g_tpl.squares.size(), g_tpl.yards.size(),
           now() - t0);
}

// the outline of a set of cells as rings in corner coordinates (each cell's edges, shared ones cancelled, chained)
std::vector<PaintRing> traceCells(const std::set<std::pair<int, int>>& cells) {
  using E = std::pair<std::pair<int, int>, std::pair<int, int>>;
  std::map<std::pair<int, int>, std::vector<std::pair<int, int>>> next;
  std::set<E> edges;
  for (auto [x, z] : cells) {
    std::pair<int, int> c[4] = {{x, z}, {x + 1, z}, {x + 1, z + 1}, {x, z + 1}};
    for (int k = 0; k < 4; ++k) {
      E e{c[k], c[(k + 1) & 3]}, rev{c[(k + 1) & 3], c[k]};
      if (edges.count(rev)) edges.erase(rev);
      else edges.insert(e);
    }
  }
  for (const E& e : edges) next[e.first].push_back(e.second);
  std::vector<PaintRing> rings;
  while (!next.empty()) {
    auto start = next.begin()->first;
    PaintRing ring;
    auto cur = start;
    while (true) {
      auto it = next.find(cur);
      if (it == next.end() || it->second.empty()) break;
      auto to = it->second.back();
      it->second.pop_back();
      if (it->second.empty()) next.erase(it);
      ring.push_back({double(cur.first), double(cur.second)});
      cur = to;
      if (cur == start) break;
    }
    // the cells' stair steps smoothed away (a slanted template outline turned onto the map would show them as a saw
    // tooth again; owner 2026-11-14): a corner within 0.75 cells of the line between its neighbours goes, repeatedly
    for (bool changed = true; changed && ring.size() > 4;) {
      changed = false;
      for (size_t i = 0; i < ring.size() && ring.size() > 4; ++i) {
        auto a = ring[(i + ring.size() - 1) % ring.size()], b = ring[(i + 1) % ring.size()];
        if (segDist(ring[i].first, ring[i].second, a, b) < 0.75) {
          ring.erase(ring.begin() + std::ptrdiff_t(i));
          changed = true;
        }
      }
    }
    if (ring.size() >= 3) rings.push_back(std::move(ring));
  }
  return rings;
}

// pavement painted with exact edges (paint_edges.h), but only on the cells allow(cell) accepts; collects the touched
// rectangle for the rebuild and the timeline
struct Painter {
  GridData& g;
  int x0 = 1 << 30, z0 = 1 << 30, x1 = -1, z1 = -1;
  template <class F>
  std::vector<size_t> paint(const std::vector<PaintRing>& rings, int type, F allow) {
    std::vector<size_t> out;
    type &= 7;
    for (const PaintAreaCell& c : paintAreaCells(g.width, g.length, rings)) {
      size_t i = size_t(c.z) * size_t(g.width) + size_t(c.x);
      uint16_t& f = g.flags[i];
      if ((f & 0x2000) || !allow(i)) continue;
      const bool whole = (f & 0x8) && !g.paintEdges.count(paintEdgeKey(c.x, c.z));
      if (c.full) {
        g.paintEdges.erase(paintEdgeKey(c.x, c.z));
        f = uint16_t((f & 0xFF98) | 0x8 | type);
      } else if (!whole && !g.paintEdges.count(paintEdgeKey(c.x, c.z))) {
        PaintEdge e = c.edge;
        e.type = uint8_t(type);
        g.paintEdges[paintEdgeKey(c.x, c.z)] = e;
      } else {
        continue;
      }
      out.push_back(i);
      x0 = std::min(x0, c.x), z0 = std::min(z0, c.z), x1 = std::max(x1, c.x), z1 = std::max(z1, c.z);
    }
    return out;
  }
};

// the template of `lib` that covers the target cells best (IoU at least minIoU), its furniture placed on the target
// cells where ok(used) allows; false when none fits
template <class F>
bool placeTemplate(VM& vm, GridObj* grid, Apply& a, const std::vector<TplArea>& lib, const std::vector<std::pair<int, int>>& target,
                   double minIoU, F ok, const char* what, std::vector<std::pair<std::vector<PaintRing>, int>>* paintOut = nullptr) {
  if (lib.empty() || target.size() < 10) return false;
  const int W = grid->data->width;
  std::set<std::pair<int, int>> S(target.begin(), target.end());
  double cx = 0, cz = 0;
  std::vector<std::pair<double, double>> pts;
  for (auto& c : target) cx += c.first + 0.5, cz += c.second + 0.5, pts.push_back({c.first + 0.5, c.second + 0.5});
  cx /= double(target.size()), cz /= double(target.size());
  const double angS = cellsAxis(pts);
  struct Fit {
    const TplArea* t;
    double th, iou;
  };
  std::vector<Fit> fits;
  double best = 0;
  for (const TplArea& t : lib) {
    const double n = double(t.cells.size()), m = double(target.size());
    if (n < 0.6 * m || n > 1.7 * m) continue;
    for (int k = 0; k < 4; ++k) {
      const double th = angS - t.ang + k * kPi / 2, c = std::cos(th), s = std::sin(th);
      int inter = 0;
      for (auto& p : pts) {
        double dx = p.first - cx, dz = p.second - cz;
        double qx = t.cx + dx * c + dz * s, qz = t.cz - dx * s + dz * c;  // turned back by th
        inter += t.cells.count({int(std::floor(qx)), int(std::floor(qz))}) != 0;
      }
      double iou = inter / (n + m - inter);
      if (iou >= minIoU) fits.push_back({&t, th, iou}), best = std::max(best, iou);
    }
  }
  if (fits.empty()) return false;
  std::vector<const Fit*> good;
  for (const Fit& f : fits)
    if (f.iou >= best - 0.05) good.push_back(&f);
  const Fit& f = *good[hash32(uint64_t(std::llround(cx * 16)) * 92821u + uint64_t(std::llround(cz * 16))) % good.size()];
  const double c = std::cos(f.th), s = std::sin(f.th);
  int placed = 0;
  if (paintOut && !f.t->paint.empty()) {  // the template's pavement: its outlines per paint, turned onto the target
    std::map<int, std::set<std::pair<int, int>>> byType;
    for (auto& [cell, t] : f.t->paint) byType[t - 1].insert(cell);
    for (auto& [type, cells] : byType) {
      std::vector<PaintRing> rings = traceCells(cells);
      for (PaintRing& r : rings)
        for (auto& q : r) {
          double dx = q.first - f.t->cx, dz = q.second - f.t->cz;
          q = {cx + dx * c - dz * s, cz + dx * s + dz * c};
        }
      paintOut->push_back({std::move(rings), type});
    }
  }
  for (const TplProp& p : f.t->props) {
    double px = cx + p.x * c - p.z * s, pz = cz + p.x * s + p.z * c;
    if (!S.count({int(std::floor(px)), int(std::floor(pz))})) continue;
    double fx = p.fx * c - p.fz * s, fz = p.fx * s + p.fz * c;
    if (placeAt(vm, grid, a, p.name, px, pz, fx, fz, ok, 6) >= 0) ++placed;
  }
  if (std::getenv("REGS_DEBUG_OSM"))
    LOG_INFO("osm: %s of %zu cells at world %.0f, %.0f from a template of %s (%zu cells, IoU %.2f): %d of %zu things", what,
             target.size(), (cx - W * 0.5) * grid->data->cellSize, (cz - grid->data->length * 0.5) * grid->data->cellSize,
             f.t->from.c_str(), f.t->cells.size(), f.iou, placed, f.t->props.size());
  return placed > 0;
}

// ---- furnishing (owner 2026-11-14: fountains on squares, street lamps, courtyards and squares decorated, car parks
// with rows of bays, more of OSM's data used) ----
bool insideRing(const std::vector<std::pair<double, double>>& p, double x, double z) {
  int n = 0;
  for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++)
    if ((p[i].second <= z) != (p[j].second <= z) &&
        x < p[i].first + (z - p[i].second) * (p[j].first - p[i].first) / (p[j].second - p[i].second))
      ++n;
  return (n & 1) != 0;
}

struct Furnish {
  std::vector<std::string> trees, bushes, bigTrees;
  std::string pick(const std::vector<std::string>& v, uint32_t h) const { return v.empty() ? std::string() : v[h % v.size()]; }
};

// a tree or bush in a cell, turned and shifted freely
bool plantAt(VM& vm, GridObj* grid, Apply& a, const std::string& name, int x, int z, uint32_t h) {
  GridData& g = *grid->data;
  if (name.empty()) return false;
  int made = addItem(vm, grid, a, name, x, meanCorner(g, x, z), z, int((h >> 8) & 3));
  if (made < 0) return false;
  GridData::Item& it = g.items[size_t(made)];
  const float half = g.cellSize * 0.35f;
  it.yaw = (float((h >> 3) % 1000) / 1000.0f - 0.5f) * 1.5707963f;
  it.offX = (float((h >> 13) % 1000) / 1000.0f * 2.0f - 1.0f) * half;
  it.offZ = (float((h >> 23) % 512) / 512.0f * 2.0f - 1.0f) * half;
  a.used[size_t(z) * size_t(g.width) + size_t(x)] = 4;
  ++a.treesPlaced;
  return true;
}

// the first of the names the map has
std::string firstOf(GridObj* grid, std::initializer_list<const char*> names) {
  for (const char* n : names)
    if (grid->templates.count(n)) return n;
  return {};
}

// OSM's street furniture where it is mapped; benches, kiosks and the like face the street they stand by, lamps reach
// over it; points on the carriageway move to its kerb
void placePoints(VM& vm, GridObj* grid, Apply& a, std::vector<std::pair<double, double>>& lamps) {
  const ImportData& d = *a.d;
  int made = 0;
  auto ok = [](uint8_t u) { return u != 1 && u != 3 && u != 4 && u != 6 && u != 7; };
  for (const PointIn& pt : d.points) {
    double px = pt.x, pz = pt.z, tx = 0, tz = -1;  // toward the street
    const StreetIndex::P* st = g_streets.nearest(px, pz, 3.0);
    if (st) {
      double dx = px - st->x, dz = pz - st->z, dc = std::hypot(dx, dz);
      if (dc > 1e-6) {
        double nx = dx / dc, nz = dz / dc;
        if (dc < st->cw + 0.35) px = st->x + nx * (st->cw + 0.4), pz = st->z + nz * (st->cw + 0.4);
        tx = -nx, tz = -nz;
      }
    }
    double fx = tx, fz = tz;
    std::string name;
    switch (pt.kind) {
      case 0: {  // a fountain: the large one on a square with room for it
        bool big = false;
        for (const AreaIn& ar : d.areas)
          if (ar.kind != 1 && polyArea(ar.pts) >= 150 && insideRing(ar.pts, px, pz)) big = true;
        if (big && placeAt(vm, grid, a, "large-fountain", px, pz, 0, -1, ok, 6) >= 0) {
          ++made;
          continue;
        }
        name = firstOf(grid, {"garden-fountain-01", "garden-fountain-02"});
        break;
      }
      case 1: name = firstOf(grid, {"pbench-01", "gardenbench"}); break;
      case 2: name = firstOf(grid, {"street-litterbin", "litterbin-01"}); break;
      case 3: name = firstOf(grid, {"bicycle-holder-01", "bicycle-holder-02"}); break;
      case 4: name = firstOf(grid, {"phone-booth", "old-phone-booth"}); break;
      case 5: name = firstOf(grid, {"mailbox02", "mailbox01"}); break;
      case 6: name = firstOf(grid, {"public-toilet"}); break;
      case 7:
        if (st && st->kind >= 1) name = firstOf(grid, {"street-light-large", "street-light01"}), armFront(tx, tz, fx, fz);
        else if (st) name = firstOf(grid, {"street-light01", "old-streetlamp"});
        else name = firstOf(grid, {"old-streetlamp", "street-light01"}), armFront(tx, tz, fx, fz);
        break;
      case 8: name = firstOf(grid, {"standing-statue", "obelisk"}); break;
      case 9: name = firstOf(grid, {"obelisk", "rider-statue", "standing-statue"}); break;
      case 10: name = firstOf(grid, {"sculpture-01", "standing-statue"}); break;
      case 11: name = firstOf(grid, {"kiosk-02", "kiosk-01"}); break;
      case 12: name = firstOf(grid, {"street-clock"}); break;
      case 13:  // a playground: a sandbox and a swing beside it
        if (placeAt(vm, grid, a, firstOf(grid, {"sandbox"}), px, pz, fx, fz, ok, 6) >= 0) ++made;
        name = firstOf(grid, {"gardenswing"}), px += 2.5;
        break;
    }
    if (name.empty()) continue;
    if (placeAt(vm, grid, a, name, px, pz, fx, fz, ok, 6) >= 0) {
      ++made;
      if (pt.kind == 7) lamps.push_back({px, pz});
    }
  }
  if (!d.points.empty()) LOG_INFO("osm: %d of %zu furniture points placed", made, d.points.size());
}

// a square: a fountain in the middle (unless OSM maps one), benches around it facing it with litter bins between,
// flower beds, market stands on a market place, old lamps along the edge with trees between them on big squares
void decorateSquare(VM& vm, GridObj* grid, Apply& a, const AreaIn& ar, const Furnish& fu) {
  const ImportData& d = *a.d;
  GridData& g = *grid->data;
  const int W = g.width;
  const double area = polyArea(ar.pts);
  if (area < 8 || ar.pts.size() < 3) return;
  double sa = 0, cx = 0, cz = 0;
  for (size_t i = 0, j = ar.pts.size() - 1; i < ar.pts.size(); j = i++) {
    double cr = ar.pts[j].first * ar.pts[i].second - ar.pts[i].first * ar.pts[j].second;
    sa += cr, cx += (ar.pts[j].first + ar.pts[i].first) * cr, cz += (ar.pts[j].second + ar.pts[i].second) * cr;
  }
  if (std::fabs(sa) < 1e-9) return;
  cx /= 3 * sa, cz /= 3 * sa;
  if (!insideRing(ar.pts, cx, cz)) {  // a ring shape: the inside cell nearest its centroid
    double best = 1e30, bx = cx, bz = cz;
    for (auto& q : ar.pts)
      for (int k = 1; k <= 3; ++k) {
        double x = q.first + (cx - q.first) * k / 6.0, z = q.second + (cz - q.second) * k / 6.0;
        if (insideRing(ar.pts, x, z) && std::hypot(x - cx, z - cz) < best) best = std::hypot(x - cx, z - cz), bx = x, bz = z;
      }
    if (best > 1e29) return;
    cx = bx, cz = bz;
  }
  auto okIn = [&](uint8_t u) { return u == 0 || u == 5; };
  {  // a square of the game's own maps that fits it
    std::vector<std::pair<int, int>> cells;
    double x0 = 1e30, z0 = 1e30, x1 = -1e30, z1 = -1e30;
    for (auto& q : ar.pts) x0 = std::min(x0, q.first), z0 = std::min(z0, q.second), x1 = std::max(x1, q.first), z1 = std::max(z1, q.second);
    for (int z = std::max(1, int(z0)); z <= std::min(g.length - 2, int(z1)); ++z)
      for (int x = std::max(1, int(x0)); x <= std::min(W - 2, int(x1)); ++x)
        if (insideRing(ar.pts, x + 0.5, z + 0.5) && okIn(a.used[size_t(z) * size_t(W) + size_t(x)])) cells.push_back({x, z});
    if (ar.kind == 0 && placeTemplate(vm, grid, a, g_tpl.squares, cells, 0.5, okIn, "square")) return;
  }
  const uint32_t h = hash32(uint64_t(std::llround(cx * 64)) * 1000003ull + uint64_t(std::llround(cz * 64)));
  bool osmFountain = false;
  for (const PointIn& p : d.points)
    if (p.kind == 0 && insideRing(ar.pts, p.x, p.z)) osmFountain = true;
  double R = 0;
  if (!osmFountain && ar.kind == 0) {
    if (area >= 150 && placeAt(vm, grid, a, "large-fountain", cx, cz, 0, -1, okIn, 6) >= 0) R = 2.5;
    else if (area >= 20 && placeAt(vm, grid, a, firstOf(grid, {"garden-fountain-01", "garden-fountain-02"}), cx, cz, 0, -1, okIn, 6) >= 0)
      R = 0.6;
  }
  auto inside = [&](double x, double z) { return insideRing(ar.pts, x, z); };
  if (R > 0) {
    const int n = std::clamp(int(area / 40), 4, 8);
    const double rad = R + 2.3, off = (h % 2) ? kPi / n : 0;
    for (int k = 0; k < n; ++k) {
      double an = 2 * kPi * k / n + off, px = cx + std::cos(an) * rad, pz = cz + std::sin(an) * rad;
      if (!inside(px, pz)) continue;
      placeAt(vm, grid, a, firstOf(grid, {"pbench-01", "gardenbench"}), px, pz, cx - px, cz - pz, okIn, 6);
      if (k % 2 == 0) {
        double qx = px - std::sin(an) * 1.3, qz = pz + std::cos(an) * 1.3;
        if (inside(qx, qz)) placeAt(vm, grid, a, firstOf(grid, {"street-litterbin", "litterbin-01"}), qx, qz, cx - qx, cz - qz, okIn, 6);
      }
    }
    if (area >= 80)
      for (int k = 0; k < 4; ++k) {
        double an = kPi / 4 + kPi / 2 * k + off, px = cx + std::cos(an) * (R + 1.1), pz = cz + std::sin(an) * (R + 1.1);
        if (inside(px, pz))
          placeAt(vm, grid, a, firstOf(grid, {k % 2 ? "street-flowerbed-02" : "street-flowerbed-01", "flowerbed-01"}), px, pz, cx - px,
                  cz - pz, okIn, 6);
      }
  }
  if (ar.kind == 2) {  // a market place: stands on rings around the middle
    static const char* kStands[] = {"marketstand01", "marketstand02", "marketstand03", "marketstand04", "marketumbrella",
                                    "markettableset", "marketstand01", "marketstand02"};
    for (double rad = R + 3.5; rad < std::sqrt(area) * 0.6; rad += 3.5)
      for (int k = 0, n = std::max(4, int(2 * kPi * rad / 3.4)); k < n; ++k) {
        double an = 2 * kPi * k / n, px = cx + std::cos(an) * rad, pz = cz + std::sin(an) * rad;
        if (inside(px, pz)) placeAt(vm, grid, a, firstOf(grid, {kStands[hash32(h + k * 7 + uint32_t(rad)) % 8]}), px, pz, cx - px, cz - pz, okIn, 6);
      }
  }
  // the edge: old lamps every 6 cells a cell and a half inside, trees between them on big squares
  double perim = 0;
  for (size_t i = 0; i < ar.pts.size(); ++i) {
    auto p = ar.pts[i], q = ar.pts[(i + 1) % ar.pts.size()];
    perim += std::hypot(q.first - p.first, q.second - p.second);
  }
  const std::string lamp = firstOf(grid, {"old-streetlamp", "street-light01"});
  int k = 0;
  for (double s = 3.0; s < perim; s += 3.0, ++k) {
    double acc = 0;
    for (size_t i = 0; i < ar.pts.size(); ++i) {
      auto p = ar.pts[i], q = ar.pts[(i + 1) % ar.pts.size()];
      double l = std::hypot(q.first - p.first, q.second - p.second);
      if (acc + l < s || l < 1e-9) {
        acc += l;
        continue;
      }
      double t = (s - acc) / l, ex = (q.first - p.first) / l, ez = (q.second - p.second) / l;
      double bx = p.first + (q.first - p.first) * t, bz = p.second + (q.second - p.second) * t;
      double nx = -ez, nz = ex;
      if (!inside(bx + nx * 1.5, bz + nz * 1.5)) nx = -nx, nz = -nz;
      double px = bx + nx * 1.5, pz = bz + nz * 1.5;
      if (inside(px, pz)) {
        if (k % 2 == 0) {
          double fx, fz;
          armFront(nx, nz, fx, fz);
          placeAt(vm, grid, a, lamp, px, pz, fx, fz, okIn, 6);
        } else if (area >= 200) {
          int x = int(std::floor(px)), z = int(std::floor(pz));
          size_t ci = size_t(z) * size_t(W) + size_t(x);
          if (okIn(a.used[ci])) plantAt(vm, grid, a, fu.pick(fu.trees, h + k), x, z, hash32(h * 3 + k));
        }
      }
      break;
    }
  }
}

// a car park: asphalt (painted at the terrain stage) and rows of bays across its long side: bay, aisle, two bays back
// to back, aisle, …; a bay only where it lies wholly inside the car park on free ground
int decorateParking(VM& vm, GridObj* grid, Apply& a, const AreaIn& ar, int budget) {
  double ang, ea, eb, cx, cz;
  if (budget <= 0 || polyArea(ar.pts) < 6 || !minRect(ar.pts, ang, ea, eb, cx, cz)) return 0;
  double ux = std::cos(ang), uz = std::sin(ang), len = ea, wid = eb;
  if (eb > ea) ux = -std::sin(ang), uz = std::cos(ang), len = eb, wid = ea;
  const double vx = -uz, vz = ux;
  const std::string bay = firstOf(grid, {"parking-space"});
  if (bay.empty() || wid < 2.2) return 0;
  std::vector<double> rowsAt;  // the bay rows' centres across
  double cur = -wid * 0.5 + 0.2;
  rowsAt.push_back(cur + 1.0), cur += 2.0;
  while (cur + 2.4 + 2.0 <= wid * 0.5) {
    cur += 2.4;
    rowsAt.push_back(cur + 1.0), cur += 2.0;
    if (cur + 2.0 <= wid * 0.5 - 0.1) rowsAt.push_back(cur + 1.0), cur += 2.0;
  }
  auto ok = [](uint8_t u) { return u == 0 || u == 5; };
  int made = 0;
  for (double vo : rowsAt)
    for (double t = -len * 0.5 + 0.6; t <= len * 0.5 - 0.5 && made < budget; t += 1.0) {
      double px = cx + ux * t + vx * vo, pz = cz + uz * t + vz * vo;
      bool in = true;
      for (double su : {-0.45, 0.45})
        for (double sv : {-0.95, 0.95}) in = in && insideRing(ar.pts, px + ux * su + vx * sv, pz + uz * su + vz * sv);
      if (in && placeAt(vm, grid, a, bay, px, pz, vx, vz, ok, 6) >= 0) ++made;
    }
  return made;
}

// street lamps along the curves where OSM maps none: small streets on one side, wide roads and avenues on both
// (staggered); every 28 m (32 m on wide roads, 20 m on footpaths), clear of the junctions; the plain post on small
// streets, the large arm lamp reaching over wide roads, the old lantern by footpaths
void streetLamps(VM& vm, GridObj* grid, Apply& a, const std::vector<std::pair<double, double>>& osmLamps) {
  GridData& g = *grid->data;
  const int W = g.width, L = g.length;
  const float cs = g.cellSize;
  std::map<std::pair<int, int>, std::vector<std::pair<double, double>>> near;
  for (auto& q : osmLamps) near[{int(q.first) / 8, int(q.second) / 8}].push_back(q);
  auto osmNear = [&](double x, double z) {
    for (int bz = int(z) / 8 - 1; bz <= int(z) / 8 + 1; ++bz)
      for (int bx = int(x) / 8 - 1; bx <= int(x) / 8 + 1; ++bx) {
        auto it = near.find({bx, bz});
        if (it != near.end())
          for (auto& q : it->second)
            if (std::hypot(q.first - x, q.second - z) < 6.0) return true;
      }
    return false;
  };
  const std::vector<CurveJunction> js = findCurveJunctions(g);
  const std::string post = firstOf(grid, {"street-light01", "old-streetlamp"});
  const std::string arm = firstOf(grid, {"street-light-large", "street-light01"});
  const std::string lantern = firstOf(grid, {"old-streetlamp", "street-light01"});
  auto ok = [](uint8_t u) { return u != 1 && u != 3 && u != 4 && u != 6 && u != 7; };
  int made = 0;
  for (size_t ci = 0; ci < g.curves.size(); ++ci) {
    const CurveRoad& c = g.curves[ci];
    if (c.bridge || curveIsTrack(c.kind)) continue;
    const CurveProfile& pr = curveProfile(c.kind);
    float trim0 = 0, trim1 = 0;
    curveTrims(js, int(ci), trim0, trim1);
    std::vector<CurveSample> all = sampleCurve(c, 4.0f);
    if (all.empty()) continue;
    const float len = all.back().s;
    const double metres = c.kind == 3 ? 20.0 : c.kind == 0 ? 28.0 : 32.0;
    const float step = float(metres / a.d->cellM) * cs;
    const float s0 = trim0 + 4.0f, s1 = len - trim1 - 4.0f;
    if (s1 - s0 < step * 0.5f) continue;
    const float off = c.kind == 3 ? pr.halfWidth + 1.2f : pr.carriageway + 0.7f;
    const bool both = c.kind == 1 || c.kind == 2;
    const uint32_t hc = hash32(ci * 7919 + 3);
    const float oneSide = (hc & 1) ? 1.0f : -1.0f;
    for (int pass = 0; pass < (both ? 2 : 1); ++pass) {
      const float side = both ? (pass ? -1.0f : 1.0f) : oneSide;
      const float a0 = s0 + (pass ? step * 0.5f : 0.0f);
      if (a0 > s1) continue;
      for (const CurveSample& sm : sampleCurveRange(c, a0, s1, step)) {
        double wx = sm.p.x + sm.right.x * off * side, wz = sm.p.z + sm.right.z * off * side;
        double px = wx / cs + W * 0.5, pz = wz / cs + L * 0.5;
        if (osmNear(px, pz)) continue;
        double fx = 0, fz = -1;
        const std::string* name = &post;
        if (c.kind == 3) name = &lantern;
        else if (c.kind >= 1) name = &arm, armFront(-sm.right.x * side, -sm.right.z * side, fx, fz);
        int y = int(std::lround(sm.p.y * 256.0f / cs));
        if (placeAt(vm, grid, a, *name, px, pz, fx, fz, ok, 6, y) >= 0) ++made;
      }
    }
  }
  LOG_INFO("osm: %d street lamps", made);
}

// the yards behind the rows of houses: the free ground reached from behind the rows (all of it when houses and streets
// close it within 6000 cells, else the 6 cells behind the houses); bushes along
// the houses' backs, trees in the open, and the residents' things: sandboxes, swings, sheds, bicycle stands, benches,
// flower beds, now and then a garden fountain, a gazebo or a statue
void decorateYards(VM& vm, GridObj* grid, Apply& a, const Furnish& fu, Painter& painter) {
  GridData& g = *grid->data;
  const int W = g.width, L = g.length;
  std::vector<uint8_t> seen(size_t(W) * size_t(L), 0);
  static const std::pair<const char*, int> kProps[] = {
      {"sandbox", 2},      {"gardenswing", 2},       {"garden-shed", 2},        {"bicycle-holder-01", 3}, {"bicycle-holder-02", 2},
      {"litterbin-01", 2}, {"litterbin-02", 1},      {"flowerbed-01", 3},       {"flowerbed-02", 3},      {"gardenbench", 3},
      {"pbench-01", 2},    {"garden-fountain-01", 1}, {"garden-fountain-02", 1}, {"gazebo", 1},            {"garden-statue-01", 1},
      {"gnome", 1},        {"doghouse", 1},           {"flowerpot-01", 2},       {"flowerpot-02", 1}};
  std::vector<std::string> pool;
  for (auto& [n, wgt] : kProps)
    if (grid->templates.count(n))
      for (int k = 0; k < wgt; ++k) pool.push_back(n);
  auto at = [&](int x, int z) { return a.used[size_t(z) * size_t(W) + size_t(x)]; };
  auto clear = [&](int x, int z, int r, std::initializer_list<uint8_t> bad) {
    for (int dz = -r; dz <= r; ++dz)
      for (int dx = -r; dx <= r; ++dx) {
        int xx = x + dx, zz = z + dz;
        if (xx < 0 || zz < 0 || xx >= W || zz >= L) continue;
        for (uint8_t b : bad)
          if (at(xx, zz) == b) return false;
      }
    return true;
  };
  int yards = 0, things = 0, tpls = 0;
  // the bands of pavement behind the rows first (on free ground and under the yards' things)
  std::set<size_t> paved;
  auto groundOk = [&](size_t i) { uint8_t u = a.used[i]; return u == 0 || u == 4 || u == 6; };
  for (auto& band : a.yardBands)
    for (size_t i : painter.paint({band}, 0, groundOk)) paved.insert(i);
  for (auto& sd : a.yardSeeds) {
    int sx = int(std::floor(sd.first)), sz = int(std::floor(sd.second));
    if (sx < 1 || sz < 1 || sx >= W - 1 || sz >= L - 1 || seen[size_t(sz) * size_t(W) + size_t(sx)] || at(sx, sz)) continue;
    std::vector<std::pair<int, int>> cells;
    std::vector<std::pair<std::pair<int, int>, int>> q{{{sx, sz}, 0}};
    seen[size_t(sz) * size_t(W) + size_t(sx)] = 1;
    // the free ground around (up to 6000 cells; more: not a yard closed by houses and streets but open land)
    bool open = false;
    for (size_t qi = 0; qi < q.size(); ++qi) {
      auto [p, dist] = q[qi];
      if (q.size() >= 6000) {
        open = true;
        if (dist >= 6) continue;
      }
      for (auto [nx, nz] : {std::pair{p.first - 1, p.second}, {p.first + 1, p.second}, {p.first, p.second - 1}, {p.first, p.second + 1}}) {
        if (nx < 1 || nz < 1 || nx >= W - 1 || nz >= L - 1) continue;
        size_t ni = size_t(nz) * size_t(W) + size_t(nx);
        if (seen[ni] || a.used[ni]) continue;
        seen[ni] = 1;
        q.push_back({{nx, nz}, dist + 1});
      }
    }
    // an open one: just the strip behind the houses (6 cells) is the residents' garden
    for (auto& [p, dist] : q)
      if (!open || dist <= 6) cells.push_back(p);
    if (cells.size() < 10) continue;
    ++yards;
    // a closed yard: one of the game's own yards that fits it first (then just the bushes along the houses)
    // the yard's pavement (owner 2026-11-14: yards paved): the band along the houses (above) and the template's,
    // its outlines turned onto the yard with exact edges, kept to the yard's own cells
    std::vector<std::pair<std::vector<PaintRing>, int>> tplPaint;
    const bool fromTpl = !open && placeTemplate(vm, grid, a, g_tpl.yards, cells, 0.4, [](uint8_t u) { return u == 0; }, "yard", &tplPaint);
    tpls += fromTpl;
    {
      std::set<size_t> mine;
      for (auto [x, z] : cells) mine.insert(size_t(z) * size_t(W) + size_t(x));
      for (auto& [rings, type] : tplPaint)
        for (size_t i : painter.paint(rings, type, [&](size_t i) { return mine.count(i) != 0; })) paved.insert(i);
    }
    const std::set<size_t>& pavedHere = paved;
    for (size_t k = cells.size() - 1; k > 0; --k) std::swap(cells[k], cells[hash32(k * 2654435761u + cells[0].first) % (k + 1)]);
    for (auto [x, z] : cells) {
      if (at(x, z)) continue;
      uint32_t h = hash32((uint64_t(x) << 32) | uint32_t(z) | 0x5a5a0000ull);
      bool byHouse = at(x - 1, z) == 3 || at(x + 1, z) == 3 || at(x, z - 1) == 3 || at(x, z + 1) == 3;
      if (pavedHere.count(size_t(z) * size_t(W) + size_t(x))) {
        if (!fromTpl && (h >> 7) % 100 < 5 && !pool.empty() && clear(x, z, 1, {6})) {  // bicycle stands and bins on the pavement
          static const char* kPaved[] = {"bicycle-holder-01", "bicycle-holder-02", "litterbin-01", "litterbin-02"};
          int dir = int((h >> 17) & 3);
          static const double kDir2[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
          if (placeAt(vm, grid, a, firstOf(grid, {kPaved[(h >> 9) % 4]}), x + 0.5, z + 0.5, kDir2[dir][0], kDir2[dir][1],
                      [](uint8_t u) { return u == 0; }, 6) >= 0)
            ++things;
        }
        continue;
      }
      if (byHouse) {
        if (h % 100 < 22) plantAt(vm, grid, a, fu.pick(fu.bushes, h >> 5), x, z, h);
      } else if (fromTpl) {
        continue;
      } else if (h % 100 < 7 && clear(x, z, 2, {3, 4})) {
        plantAt(vm, grid, a, fu.pick(fu.trees, h >> 5), x, z, h);
      } else if ((h >> 7) % 100 < 6 && !pool.empty() && clear(x, z, 1, {6})) {
        int dir = int((h >> 17) & 3);
        static const double kDir[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
        if (placeAt(vm, grid, a, pool[(h >> 9) % pool.size()], x + 0.5, z + 0.5, kDir[dir][0], kDir[dir][1],
                    [](uint8_t u) { return u == 0; }, 6) >= 0)
          ++things;
      }
    }
  }
  LOG_INFO("osm: %d yards furnished (%d from the game's maps, %d more things)", yards, tpls, things);
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

// the railway stations (owner 2026-11-13, 2026-11-14: "no real station"), built as the game's own maps build them
// (Berlin's station: rows of the platform pieces along the tracks, two rows back to back for a platform, lamps on it):
// along the nearest railway curve, 48 cells long, a platform by the line on the station's side, the station building
// (railway-station, 14 × 5) behind it with its back to the line, and an island platform between the line and a
// parallel one 6.5-14 cells away; platform lamps every 6 cells and benches every 12 on the platforms
void placeStations(VM& vm, GridObj* grid, Apply& a, const std::vector<StationIn>& stations) {
  GridData& g = *grid->data;
  const int W = g.width, L = g.length;
  if (stations.empty()) return;
  const float cs = g.cellSize;
  struct RP {
    double x, z, tx, tz;
    int ci;
  };
  std::vector<RP> rail;
  for (size_t ci = 0; ci < g.curves.size(); ++ci) {
    const CurveRoad& c = g.curves[ci];
    if (c.kind != 6) continue;
    for (const CurveSample& sm : sampleCurve(c, 2.0f)) {
      double l = std::hypot(sm.dir.x, sm.dir.z);
      if (l > 1e-4) rail.push_back({sm.p.x / cs + W * 0.5, sm.p.z / cs + L * 0.5, sm.dir.x / l, sm.dir.z / l, int(ci)});
    }
  }
  const std::string building = firstOf(grid, {"railway-station"}), piece = firstOf(grid, {"platform"}),
                    lamp = firstOf(grid, {"platformlamp01", "platformlamp02", "street-light01"}), bench = firstOf(grid, {"pbench-01"});
  auto free = [](uint8_t u) { return u == 0 || u == 4 || u == 5 || u == 6; };
  auto any = [](uint8_t u) { return u != 1; };
  for (const StationIn& st : stations) {
    const RP* best = nullptr;
    double bd = 20.0;
    for (const RP& r : rail) {
      double d = std::hypot(r.x - st.x, r.z - st.z);
      if (d < bd) bd = d, best = &r;
    }
    if (!best) continue;
    const double ux = best->tx, uz = best->tz, vx0 = -uz, vz0 = ux;
    const double sgn = (st.x - best->x) * vx0 + (st.z - best->z) * vz0 >= 0 ? 1.0 : -1.0;  // the station's side
    const double vx = vx0 * sgn, vz = vz0 * sgn;  // across, to the station
    const double len = 48.0;
    // the line's points along the platform (the curve itself, not a straight line: it may bend a little)
    auto lineAt = [&](double t, double& lx, double& lz) {
      double px = best->x + ux * t, pz = best->z + uz * t, bd2 = 1e30;
      lx = px, lz = pz;
      for (const RP& r : rail) {
        if (r.ci != best->ci && std::fabs(r.tx * ux + r.tz * uz) < 0.95) continue;
        double d = std::hypot(r.x - px, r.z - pz);
        double side = (r.x - best->x) * vx + (r.z - best->z) * vz;
        if (d < bd2 && std::fabs(side) < 1.5) bd2 = d, lx = r.x, lz = r.z;
      }
    };
    // a parallel line on the far side (an island platform between them)
    double far = 0;
    for (const RP& r : rail) {
      if (std::fabs(r.tx * ux + r.tz * uz) < 0.97) continue;
      double dx = r.x - best->x, dz = r.z - best->z, along = dx * ux + dz * uz, across = -(dx * vx + dz * vz);
      if (std::fabs(along) < len * 0.5 && across > 5.5 && across < 14.0 && (far == 0 || across < far)) far = across;
    }
    int pieces = 0;
    auto row = [&](double off, double fdir) {  // a row of platform pieces off cells across, fronts to fdir · v
      for (double t = -len * 0.5 + 2.0; t <= len * 0.5 - 2.0; t += 4.0) {
        double lx, lz;
        lineAt(t, lx, lz);
        if (placeAt(vm, grid, a, piece, lx + vx * off, lz + vz * off, vx * fdir, vz * fdir, free, 3) >= 0) ++pieces;
      }
    };
    // the platform by the line: two rows, 2.5 and 3.5 cells off the line's centre
    row(2.5, -1.0);
    row(3.5, 1.0);
    if (far > 0) {
      row(-far * 0.5 + 0.5, 1.0);
      row(-far * 0.5 - 0.5, -1.0);
    }
    // the station building behind the platform, its back to the line, in the middle of it
    double cx, cz;
    lineAt(0, cx, cz);
    const double bx = cx + vx * 6.5, bz = cz + vz * 6.5;
    if (!building.empty() && placeAt(vm, grid, a, building, bx, bz, vx, vz, free, 3) >= 0) {
      ++a.stationsPlaced;
      a.stationFrames.push_back({bx, bz, vx, vz});
    }
    // lamps and benches on the platforms
    for (double t = -len * 0.5 + 3.0; t <= len * 0.5 - 3.0; t += 6.0) {
      double lx, lz;
      lineAt(t, lx, lz);
      for (double off : far > 0 ? std::vector<double>{3.0, -far * 0.5} : std::vector<double>{3.0}) {
        bool benchHere = int(std::lround((t + len * 0.5) / 6.0)) % 2 == 1;
        double fx, fz;
        armFront(-vx, -vz, fx, fz);
        placeAt(vm, grid, a, benchHere ? bench : lamp, lx + vx * off, lz + vz * off, benchHere ? -vx : fx, benchHere ? -vz : fz, any, 0);
      }
    }
    LOG_INFO("osm: station '%s' built: %d platform pieces%s", st.name.c_str(), pieces, far > 0 ? ", with an island platform" : "");
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

// debugging (REGS_DEBUG_OSM_RAILS=<png>): the map at a pixel per cell: water blue, OSM's railways (after merging) yellow,
// the railway curves red, other curves grey, stations green
// ---- level crossings (owner 2026-11-15: "Bahnübergänge") ----
// Where a railway crosses a small road on the ground, the game's level crossing goes in: track_double_cross_small (4 × 4
// cells, the road along its local x with its lanes at z ±1.5, the double track along z at x ±1.5, barriers that lower
// while a train holds the crossing, and the bell). The piece is turned freely to the track (orient and the port's yaw);
// the track is cut 6 units each side of the crossing and its two halves end on the piece; the road is cut further off
// and joined to the piece's road sides by short connector curves that bend it square to the track (the piece crosses
// at right angles). Large roads, avenues, streets with tram tracks (the piece has none), bridges and crossings flatter
// than 30° are left as they are.
struct LevelCrossing {
  render::Vec3 p;  // centre (world units, y = the piece's height)
  render::Vec3 t;  // the track's direction there (flat, unit)
};

std::vector<LevelCrossing> cutLevelCrossings(std::vector<CurveRoad>& cs) {
  using render::Vec3;
  std::vector<LevelCrossing> out;
  int embedded = 0, skipped = 0;
  std::set<std::pair<long long, long long>> rejected;  // crossings already looked at and left (by point)
  auto flat = [](Vec3 v) {
    v.y = 0;
    return render::normalize(v);
  };
  auto dot2 = [](const Vec3& a, const Vec3& b) { return a.x * b.x + a.z * b.z; };
  struct Box {
    float x0, z0, x1, z1;
  };
  auto boxOf = [](const std::vector<CurveSample>& sm, float grow) {
    Box b{1e30f, 1e30f, -1e30f, -1e30f};
    for (const CurveSample& k : sm)
      b.x0 = std::min(b.x0, k.p.x), b.z0 = std::min(b.z0, k.p.z), b.x1 = std::max(b.x1, k.p.x), b.z1 = std::max(b.z1, k.p.z);
    b.x0 -= grow, b.z0 -= grow, b.x1 += grow, b.z1 += grow;
    return b;
  };
  for (int guard = 0; guard < 2000; ++guard) {
    // the first crossing of a track and a road not yet looked at
    int ti = -1, ri = -1;
    float sT = 0, sR = 0;
    Vec3 P{};
    std::vector<std::vector<CurveSample>> smp(cs.size());
    std::vector<Box> box(cs.size());
    for (size_t i = 0; i < cs.size(); ++i)
      if (!cs[i].bridge && (cs[i].kind == 6 || cs[i].kind <= 2)) smp[i] = sampleCurve(cs[i], 1.0f), box[i] = boxOf(smp[i], 0.5f);
    for (size_t i = 0; i < cs.size() && ti < 0; ++i) {
      if (cs[i].kind != 6 || cs[i].bridge) continue;
      const auto& A = smp[i];
      for (size_t j = 0; j < cs.size() && ti < 0; ++j) {
        if (cs[j].kind > 2 || cs[j].bridge || smp[j].empty()) continue;
        if (box[i].x1 < box[j].x0 || box[j].x1 < box[i].x0 || box[i].z1 < box[j].z0 || box[j].z1 < box[i].z0) continue;
        const auto& B = smp[j];
        for (size_t a = 0; a + 1 < A.size() && ti < 0; ++a)
          for (size_t b = 0; b + 1 < B.size(); ++b) {
            const Vec3 &p0 = A[a].p, &p1 = A[a + 1].p, &q0 = B[b].p, &q1 = B[b + 1].p;
            float dx = p1.x - p0.x, dz = p1.z - p0.z, ex = q1.x - q0.x, ez = q1.z - q0.z;
            float den = dx * ez - dz * ex;
            if (std::fabs(den) < 1e-9f) continue;
            float u = ((q0.x - p0.x) * ez - (q0.z - p0.z) * ex) / den, v = ((q0.x - p0.x) * dz - (q0.z - p0.z) * dx) / den;
            if (u < 0 || u >= 1 || v < 0 || v >= 1) continue;
            Vec3 X{p0.x + dx * u, p0.y + (p1.y - p0.y) * u, p0.z + dz * u};
            std::pair<long long, long long> key{std::llround(X.x * 4), std::llround(X.z * 4)};
            if (rejected.count(key)) continue;
            ti = int(i), ri = int(j), P = X;
            sT = A[a].s + (A[a + 1].s - A[a].s) * u, sR = B[b].s + (B[b + 1].s - B[b].s) * v;
            rejected.insert(key);  // looked at now (taken or left)
            break;
          }
      }
    }
    if (ti < 0) break;
    const CurveRoad T = cs[size_t(ti)], R = cs[size_t(ri)];
    std::vector<CurveSample> shT = curveShape(T), shR = curveShape(R);
    const float LT = curveLength(T), LR = curveLength(R);
    Vec3 t = flat(curveAt(shT, sT).dir), r = flat(curveAt(shR, sR).dir);
    const char* why = "";
    bool placed = false;
    do {  // the level crossing piece
    if (R.kind != 0 || (R.ends & 0x80)) {
      why = "not a small road";
      break;
    }
    if (R.ends & 4) {
      why = "tram street";
      break;
    }
    if (std::fabs(dot2(t, r)) > 0.866f) {
      why = "flatter than 30 deg";
      break;
    }
    // the piece's turn: its local +z along the track (x' = x·c + z·s, z' = −x·s + z·c with the angle π + orient·90° + yaw)
    const float ang = std::atan2(t.x, t.z);
    Vec3 u{std::cos(ang), 0, -std::sin(ang)};  // its local +x (the road)
    if (dot2(u, r) < 0) u = u * -1.0f;         // along the road's own direction
    const float bend = std::acos(std::clamp(std::fabs(dot2(u, r)), 0.0f, 1.0f));
    if (sT - 6.0f < 3.0f || sT + 6.0f > LT - 3.0f) {
      why = "track end";
      break;
    }
    const float y = curveAt(shT, sT).p.y;
    const Vec3 E0 = P - u * 6.0f, E1 = P + u * 6.0f;  // the piece's road sides
    // each side of the road on to the piece: cut far enough off for a connector that bends it square to the track; with
    // no room (a junction or an end right by the track) a free end becomes a short stub, a junction moves off the track
    // to 18 units from the crossing (the other streets' ends there with it) and a connector leads to it
    auto ends = [&](int side) {  // the other road curves' ends at R's end on that side
      std::vector<std::pair<int, int>> v;
      const float ex = side ? R.x1 : R.x0, ez = side ? R.z1 : R.z0;
      for (size_t i = 0; i < cs.size(); ++i) {
        if (int(i) == ri || curveJoinClass(cs[i].kind) != 0) continue;
        if (std::fabs(cs[i].x0 - ex) < 0.2f && std::fabs(cs[i].z0 - ez) < 0.2f) v.push_back({int(i), 0});
        if (std::fabs(cs[i].x1 - ex) < 0.2f && std::fabs(cs[i].z1 - ez) < 0.2f) v.push_back({int(i), 1});
      }
      return v;
    };
    auto hd = [](CurveRoad& c) { c.a = c.b = std::max(0.5f, 0.4f * std::hypot(c.x1 - c.x0, c.z1 - c.z0)); };
    struct Side {
      bool ok = false;
      CurveRoad keep;                            // R's part on this side (cut), or none
      bool hasKeep = false;
      std::vector<CurveRoad> add;                // connectors and stubs
      std::vector<std::pair<int, CurveRoad>> moved;  // other curves whose end moves with a junction
      const char* why = "";
    };
    auto makeSide = [&](int side) {
      Side S;
      const Vec3 E = side ? E1 : E0;
      // room for a junction's trim where three or more streets meet (a joint of two or a free end needs little)
      const float room = side ? LR - sR : sR, margin = ends(side).size() >= 2 ? 10.0f : 3.0f;
      for (float k : {1.0f, 0.6f, 0.35f}) {  // cut
        const float D = bend < 0.1f ? 6.0f : 6.0f + k * (8.0f + 14.0f * std::sin(bend));
        if (room - D < margin) continue;
        CurveRoad first, second;
        splitCurve(R, side ? sR + D : sR - D, first, second);
        CurveRoad part = side ? second : first;
        if (D <= 6.0f) {
          if (side) part.x0 = E.x, part.z0 = E.z, part.y0 = y, part.dx0 = u.x, part.dz0 = u.z, part.ends |= 1;
          else part.x1 = E.x, part.z1 = E.z, part.y1 = y, part.dx1 = u.x, part.dz1 = u.z, part.ends |= 2;
        } else {
          CurveRoad C = part;
          C.stops.clear();
          if (side) {
            C.x0 = E.x, C.y0 = y, C.z0 = E.z, C.dx0 = u.x, C.dz0 = u.z;
            C.x1 = part.x0, C.y1 = part.y0, C.z1 = part.z0, C.dx1 = part.dx0, C.dz1 = part.dz0;
            C.ends = uint8_t((R.ends & 0x60) | 1);
          } else {
            C.x0 = part.x1, C.y0 = part.y1, C.z0 = part.z1, C.dx0 = part.dx1, C.dz0 = part.dz1;
            C.x1 = E.x, C.y1 = y, C.z1 = E.z, C.dx1 = u.x, C.dz1 = u.z;
            C.ends = uint8_t((R.ends & 0x60) | 2);
          }
          hd(C);
          if (curveMinRadius(C) < 7.0f) continue;
          S.add.push_back(C);
        }
        S.keep = part, S.hasKeep = true, S.ok = true;
        return S;
      }
      if (R.ends & (side ? 2 : 1)) {  // on a block (a map-edge stub): it cannot move
        S.why = "block";
        return S;
      }
      std::vector<std::pair<int, int>> at = ends(side);
      CurveRoad C = R;
      C.stops.clear();
      C.ends = uint8_t((R.ends & 0x60) | (side ? 1 : 2));
      const float sg = side ? 1.0f : -1.0f;  // away from the crossing along the road
      if (at.empty()) {  // a free end: a stub 8 units on
        const Vec3 F = E + u * (8.0f * sg);
        if (side) C.x0 = E.x, C.z0 = E.z, C.x1 = F.x, C.z1 = F.z;
        else C.x0 = F.x, C.z0 = F.z, C.x1 = E.x, C.z1 = E.z;
        C.y0 = C.y1 = y, C.dx0 = C.dx1 = u.x, C.dz0 = C.dz1 = u.z;
        hd(C);
        S.add.push_back(C), S.ok = true;
        return S;
      }
      const Vec3 J{side ? R.x1 : R.x0, side ? R.y1 : R.y0, side ? R.z1 : R.z0};
      const float along = dot2(J - P, u) * sg;
      // the nearest place for the junction that leaves the connector longer than the junction's trim (the curves' own
      // junction shapes, findCurveJunctions) and the moved streets bent no tighter than their kind allows
      for (float need : {12.0f, 14.0f, 16.0f, 19.0f, 23.0f}) {
        const Vec3 J2 = J + u * (sg * std::max(0.0f, need - along));
        S.moved.clear();
        GridData tmp;
        bool bad = false;
        for (auto [ci, e] : at) {
          CurveRoad o = cs[size_t(ci)];
          if (o.ends & (e ? 2 : 1)) {
            S.why = "other on a block";
            return S;
          }
          if (e) o.x1 = J2.x, o.z1 = J2.z;
          else o.x0 = J2.x, o.z0 = J2.z;
          if (curveLength(o) < 10.0f || curveMinRadius(o) < curveProfile(o.kind).minRadius) bad = true;
          S.moved.push_back({ci, o});
          tmp.curves.push_back(o);
        }
        if (bad) {
          S.why = "other too short or bent";
          continue;
        }
        CurveRoad Cn = C;
        if (side) Cn.x0 = E.x, Cn.z0 = E.z, Cn.y0 = y, Cn.dx0 = u.x, Cn.dz0 = u.z, Cn.x1 = J2.x, Cn.z1 = J2.z, Cn.y1 = J.y, Cn.dx1 = u.x, Cn.dz1 = u.z;
        else Cn.x0 = J2.x, Cn.z0 = J2.z, Cn.y0 = J.y, Cn.dx0 = u.x, Cn.dz0 = u.z, Cn.x1 = E.x, Cn.z1 = E.z, Cn.y1 = y, Cn.dx1 = u.x, Cn.dz1 = u.z;
        hd(Cn);
        tmp.curves.push_back(Cn);
        for (const CurveJunction& j : findCurveJunctions(tmp))
          for (const CurveArm& arm : j.arms) {
            const float len = curveLength(tmp.curves[size_t(arm.curve)]);
            if (size_t(arm.curve) + 1 == tmp.curves.size() ? arm.trim > len - 1.5f : arm.trim > len - 6.0f) bad = true;
          }
        if (bad) {
          S.why = "junction too close";
          continue;
        }
        S.add.push_back(Cn), S.ok = true;
        return S;
      }
      return S;
    };
    Side S0 = makeSide(0), S1 = makeSide(1);
    bool done = S0.ok && S1.ok;
    for (auto& [ci, o] : S0.moved)  // a curve moved by both sides: no
      for (auto& [cj, o2] : S1.moved) done = done && ci != cj;
    if (done) {
      CurveRoad A, B, T1, tmid;
      splitCurve(T, sT + 6.0f, A, B);
      splitCurve(A, sT - 6.0f, T1, tmid);
      const Vec3 F0 = P - t * 6.0f, F1 = P + t * 6.0f;
      T1.x1 = F0.x, T1.z1 = F0.z, T1.y1 = y, T1.dx1 = t.x, T1.dz1 = t.z, T1.ends |= 2;
      B.x0 = F1.x, B.z0 = F1.z, B.y0 = y, B.dx0 = t.x, B.dz0 = t.z, B.ends |= 1;
      cs[size_t(ti)] = T1;
      cs.push_back(B);
      for (Side* S : {&S0, &S1})
        for (auto& [ci, o] : S->moved) cs[size_t(ci)] = o;
      std::vector<CurveRoad> fresh;
      for (Side* S : {&S0, &S1}) {
        if (S->hasKeep) fresh.push_back(S->keep);
        for (CurveRoad& c : S->add) fresh.push_back(c);
      }
      cs[size_t(ri)] = fresh[0];
      for (size_t k = 1; k < fresh.size(); ++k) cs.push_back(fresh[k]);
      out.push_back({{P.x, y, P.z}, t});
      if (std::getenv("REGS_DEBUG_OSM"))
        LOG_INFO("osm: level crossing at world %.1f, %.1f, height %.1f (bend %.0f deg, %zu streets moved)", P.x, P.z, y, bend * 57.3f,
                 S0.moved.size() + S1.moved.size());
      placed = true;
    } else {
      why = "no room";
      if (std::getenv("REGS_DEBUG_OSM"))
        LOG_INFO("osm: crossing %.0f,%.0f: road %.1f/%.1f (%x), track %.1f/%.1f, bend %.0f (%s/%s)", P.x, P.z, sR, LR, R.ends, sT,
                 LT, bend * 57.3f, S0.why, S1.why);
    }
    } while (false);
    if (placed) continue;
    // no piece (a larger road, a tram street, a flat angle, junctions right by the track): the rails let into the road,
    // the track's stretch under the roads drawn as the street tram strip (bridge 2: CurveRoad::bridge, terrain.cpp),
    // from where its bed and both rails' edges leave every road's surface (junctions included: their arms meet there)
    struct Near {
      std::vector<CurveSample> shape;
      float hw, cw;  // half width, kerb
    };
    std::vector<Near> roads;
    for (size_t i = 0; i < cs.size(); ++i) {
      const CurveRoad& c = cs[i];
      if (curveIsTrack(c.kind) || c.kind == 3 || c.bridge) continue;
      if (smp[i].empty() || box[i].x1 < P.x - 80 || box[i].x0 > P.x + 80 || box[i].z1 < P.z - 80 || box[i].z0 > P.z + 80) continue;
      roads.push_back({curveShape(c), curveProfile(c.kind).halfWidth, curveProfile(c.kind).carriageway});
    }
    // a road the track crosses: its whole width; one alongside (its pavement may brush the bed): its carriageway
    auto onRoad = [&](float sAt) {
      CurveSample k = curveAt(shT, sAt);
      const Vec3 kd = flat(k.dir);
      for (Vec3 q : {k.p, k.p + k.right * 3.0f, k.p - k.right * 3.0f})
        for (const Near& n : roads) {
          float side, ss;
          curveNearest(n.shape, q.x, q.z, side, ss);
          const bool across = std::fabs(dot2(flat(curveAt(n.shape, ss).dir), kd)) < 0.85f;
          if (std::fabs(side) < (across ? n.hw : n.cw) + 0.05f) return true;
        }
      return false;
    };
    float s0 = sT, s1 = sT;
    while (s0 > 0 && sT - s0 < 45 && onRoad(s0)) s0 -= 0.25f;
    while (s1 < LT && s1 - sT < 45 && onRoad(s1)) s1 += 0.25f;
    if (s0 < 2.0f || s1 > LT - 2.0f || sT - s0 >= 45 || s1 - sT >= 45) {
      ++skipped;
      if (std::getenv("REGS_DEBUG_OSM"))
        LOG_INFO("osm: crossing %.0f,%.0f left (%s; rails in the road: %.1f..%.1f of %.1f)", P.x, P.z, why, s0, s1, LT);
      continue;
    }
    {
      CurveRoad A, C3, A1, mid;
      splitCurve(T, s1, A, C3);
      splitCurve(A, s0, A1, mid);
      const float yr = curveAt(shR, sR).p.y;  // the road's height: the rails lie in its asphalt
      mid.bridge = 2;
      mid.y0 = mid.y1 = yr, A1.y1 = yr, C3.y0 = yr;
      cs[size_t(ti)] = A1;
      cs.push_back(mid), cs.push_back(C3);
      ++embedded;
      if (std::getenv("REGS_DEBUG_OSM"))
        LOG_INFO("osm: crossing %.0f,%.0f: rails in the road over %.1f units (%s)", P.x, P.z, s1 - s0, why);
    }
  }
  LOG_INFO("osm: level crossings: %zu with barriers, %d with the rails let into the road, %d left", out.size(), embedded, skipped);
  return out;
}

// the level crossings' pieces, the ground levelled under them and their cells kept free (track cells)
void placeLevelCrossings(VM& vm, GridObj* grid, Apply& a, const std::vector<LevelCrossing>& lc) {
  if (lc.empty()) return;
  GridData& g = *grid->data;
  const int W = g.width, L = g.length;
  const float cs = g.cellSize;
  gridEditBegin(vm, grid);
  int x0 = 1 << 30, z0 = 1 << 30, x1 = -1, z1 = -1, made = 0;
  for (const LevelCrossing& c : lc) {
    const double ang = std::atan2(double(c.t.x), double(c.t.z));
    const double q = (ang - kPi) / (kPi / 2), k = std::round(q);
    const int orient = ((int(k) % 4) + 4) % 4;
    const float yaw = float((q - k) * kPi / 2);
    const double px = c.p.x / cs + W * 0.5, pz = c.p.z / cs + L * 0.5;
    const int x = int(std::lround(px - 2)), z = int(std::lround(pz - 2));
    if (x < 1 || z < 1 || x + 5 >= W || z + 5 >= L) continue;
    const int yi = int(std::lround(c.p.y * 256.0f / cs));
    // the piece sets its 4 × 4 cells' corners to its height (the pavement's) and locks them: turned, those corners stick
    // out beside it over the streets' asphalt, so they get back the heights the streets gave them
    std::vector<int16_t> before;
    for (int zz = z; zz <= z + 4; ++zz)
      for (int xx = x; xx <= x + 4; ++xx) before.push_back(g.height[size_t(zz) * size_t(W) + size_t(xx)]);
    int it = addItem(vm, grid, a, "track_double_cross_small", x, yi, z, orient);
    if (it < 0) continue;
    for (int zz = z, k = 0; zz <= z + 4; ++zz)
      for (int xx = x; xx <= x + 4; ++xx) g.height[size_t(zz) * size_t(W) + size_t(xx)] = before[size_t(k++)];
    ++made;
    GridData::Item& item = g.items[size_t(it)];
    item.yaw = std::fabs(yaw) < 1e-4f ? 0.0f : yaw;
    item.offX = float((px - (x + 2)) * cs), item.offZ = float((pz - (z + 2)) * cs);
    // the ground: flat just under the piece's road (−0.2) over its square and a unit around it, eased out over 3 more
    const float ux = float(std::cos(ang)), uz = float(-std::sin(ang)), tx = c.t.x, tz = c.t.z;
    const int16_t target = int16_t(std::lround((c.p.y - 0.25f) * 256.0f / cs));
    for (int zz = int(pz) - 6; zz <= int(pz) + 6; ++zz)
      for (int xx = int(px) - 6; xx <= int(px) + 6; ++xx) {
        if (xx < 0 || zz < 0 || xx >= W || zz >= L) continue;
        const float wx = (float(xx) - W * 0.5f) * cs - c.p.x, wz = (float(zz) - L * 0.5f) * cs - c.p.z;  // a corner
        const float d = std::max(std::fabs(wx * ux + wz * uz), std::fabs(wx * tx + wz * tz)) - 7.0f;
        const size_t ci = size_t(zz) * size_t(W) + size_t(xx);
        const bool mine = xx >= x && xx <= x + 4 && zz >= z && zz <= z + 4;
        if (d > 3.0f || ((g.flags[ci] & 0x1000) && !mine)) continue;
        const float w = d <= 0 ? 1.0f : 1.0f - d / 3.0f;
        g.height[ci] = int16_t(std::lround(float(g.height[ci]) + (float(target) - float(g.height[ci])) * w * w * (3 - 2 * w)));
        x0 = std::min(x0, xx), z0 = std::min(z0, zz), x1 = std::max(x1, xx), z1 = std::max(z1, zz);
        // the cell (its centre in the square): a track's (no furniture, no buildings)
        const float cx = wx + cs * 0.5f, cz = wz + cs * 0.5f;
        if (std::max(std::fabs(cx * ux + cz * uz), std::fabs(cx * tx + cz * tz)) <= 7.0f) a.used[ci] = 7;
      }
  }
  if (x1 >= x0) {
    gridEditCellsChanged(grid, std::max(0, x0 - 1), std::max(0, z0 - 1), x1, z1, true);
    if (a.timeline) {
      const int pw = x1 - x0 + 1, pl = z1 - z0 + 1;
      std::vector<int16_t> hs(size_t(pw) * size_t(pl));
      std::vector<uint16_t> fs(hs.size());
      for (int zz = 0; zz < pl; ++zz)
        for (int xx = 0; xx < pw; ++xx) {
          size_t i = size_t(z0 + zz) * size_t(W) + size_t(x0 + xx);
          hs[size_t(zz) * size_t(pw) + size_t(xx)] = g.height[i];
          fs[size_t(zz) * size_t(pw) + size_t(xx)] = uint16_t(g.flags[i] & 0x203F);
        }
      timelineRecordPatch(vm, a.timeline, x0, z0, pw, pl, hs, fs);
    }
  }
  gridEditEnd(vm, grid);
  if (std::getenv("REGS_DEBUG_OSM")) LOG_INFO("osm: %d level crossing pieces placed", made);
}

void debugRails(const GridData& g, const ImportData& d, const char* path) {
  const int W = g.width, L = g.length;
  std::vector<uint8_t> px(size_t(W) * size_t(L) * 4, 0);
  auto put = [&](double x, double z, uint8_t r, uint8_t gg, uint8_t b) {
    int ix = int(std::floor(x)), iz = int(std::floor(z));
    if (ix < 0 || iz < 0 || ix >= W || iz >= L) return;
    uint8_t* q = &px[(size_t(iz) * size_t(W) + size_t(ix)) * 4];
    q[0] = r, q[1] = gg, q[2] = b, q[3] = 255;
  };
  for (int z = 0; z < L; ++z)
    for (int x = 0; x < W; ++x) {
      bool w = d.water.size() == size_t(W) * size_t(L) && d.water[size_t(z) * size_t(W) + size_t(x)];
      put(x, z, w ? 40 : 20, w ? 60 : 30, w ? 140 : 20);
    }
  for (const RoadIn& r : d.roads) {  // OSM's railways under the curves, a cell to the right
    if (r.kind != 5) continue;
    for (size_t i = 0; i + 1 < r.pts.size(); ++i) {
      auto a = r.pts[i], b = r.pts[i + 1];
      int n = int(std::hypot(b.first - a.first, b.second - a.second) * 2) + 1;
      for (int k = 0; k <= n; ++k) put(a.first + (b.first - a.first) * k / n + 1, a.second + (b.second - a.second) * k / n, 120, 110, 30);
    }
  }
  const float cs = g.cellSize;
  for (const CurveRoad& c : g.curves)
    for (const CurveSample& sm : sampleCurve(c, 1.0f)) {
      bool tr = curveIsTrack(c.kind);
      put(sm.p.x / cs + W * 0.5, sm.p.z / cs + L * 0.5, tr ? 255 : 110, tr ? (c.bridge ? 160 : 40) : 110, tr ? 40 : 110);
    }
  for (const StationIn& st : d.stations)
    for (int dz = -3; dz <= 3; ++dz)
      for (int dx = -3; dx <= 3; ++dx) put(st.x + dx, st.z + dz, 40, 230, 60);
  std::vector<uint8_t> png = render::encodePng(px.data(), W, L);
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
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
    if (!d.heights.empty() || water || !d.paving.empty() || !d.paintAreas.empty()) {
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
      // car parks and squares painted with exact edges (no saw tooth along slanted outlines; owner 2026-11-14): whole
      // cells and the border cells cut along the outline; nothing is built on either
      int painted = 0;
      for (auto& [rings, type] : d.paintAreas) {
        int px0, pz0, px1, pz1;
        for (const PaintAreaCell& c : paintArea(g, rings, type & 7, px0, pz0, px1, pz1)) {
          size_t i = size_t(c.z) * size_t(W) + size_t(c.x);
          if (d.water[i]) continue;
          if (!(type & 0x100)) a.used[i] = 5;
          flags[i] = uint16_t(g.flags[i] & kGround);
          ++painted;
        }
      }
      gridEditCellsChanged(grid, 0, 0, W - 1, L - 1, true);
      if (a.timeline) timelineRecordPatch(vm, a.timeline, 0, 0, W, L, heights, flags);
      if (a.timeline && !d.paintAreas.empty()) timelineRecordPaintEdges(vm, a.timeline, g, 0, 0, W, L);
      if (std::getenv("REGS_DEBUG_OSM")) LOG_INFO("osm: %zu areas painted with exact edges (%d cells)", d.paintAreas.size(), painted);
    }
    a.stage = 2;
    a.next = 0;
    if (now() - t0 > budget) return false;
  }
  if (a.stage == 2 && !a.gridStreets && !d.roads.empty()) {  // the streets as curves
    streets::Plan plan = streets::plan(d.roads, W, L, g.cellSize, d.water, g.height);
    streets::pruneTracks(plan, W, L, g.cellSize);
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
    // bridge decks (fixed 2026-11-14 with the curve session's check of the Bruchsal map: decks lay at the channel bed,
    // hidden by its walls): a point in a channel (water within 5 cells, its ground 2+ units under the highest ground
    // within 5 cells) makes its legs bridges; each run of joined bridge legs gets one deck: the highest of its points
    // on solid ground (the banks), at least 1 unit over the ground under it, 6 over streets an OSM bridge crosses
    {
      const float cs2 = g.cellSize;
      auto lowNode = [&](const streets::Node& n) {
        int cx = int(std::floor(n.x / cs2 + W * 0.5)), cz = int(std::floor(n.z / cs2 + L * 0.5));
        bool wet = false;
        float hi = -1e9f;
        for (int dz = -5; dz <= 5; ++dz)
          for (int dx = -5; dx <= 5; ++dx) {
            int x = cx + dx, z = cz + dz;
            if (x < 0 || z < 0 || x >= W || z >= L) continue;
            if (std::abs(dx) <= 2 && std::abs(dz) <= 2 && d.water.size() == size_t(W) * size_t(L) &&
                d.water[size_t(z) * size_t(W) + size_t(x)])
              wet = true;  // the water right by it (not a street along the bank)
            hi = std::max(hi, float(g.height[size_t(z) * size_t(W) + size_t(x)]) * cs2 / 256.0f);
          }
        return wet && groundY(g, n.x, n.z) < hi - 2.0f;
      };
      std::vector<char> low(plan.nodes.size(), 0);
      for (size_t i = 0; i < plan.nodes.size(); ++i)
        if (plan.nodes[i].alive && plan.nodes[i].exit < 0 && !plan.nodes[i].foot) low[i] = lowNode(plan.nodes[i]);
      for (streets::Seg& sg : plan.segs)
        if (sg.alive && !sg.fixed && sg.kind != 3 && (low[size_t(sg.a)] || low[size_t(sg.b)])) {
          const streets::Node &A = plan.nodes[size_t(sg.a)], &B = plan.nodes[size_t(sg.b)];
          if (std::hypot(B.x - A.x, B.z - A.z) <= 30.0) sg.bridge = true;  // short legs into the channel (long ones ramp)
        }
      auto crossesStreet = [&](const streets::Seg& br) {
        const streets::Node &A = plan.nodes[size_t(br.a)], &B = plan.nodes[size_t(br.b)];
        for (const streets::Seg& o : plan.segs) {
          if (!o.alive || o.bridge || o.a == br.a || o.a == br.b || o.b == br.a || o.b == br.b) continue;
          const streets::Node &C = plan.nodes[size_t(o.a)], &D = plan.nodes[size_t(o.b)];
          if (streets::segCross({A.x, A.z}, {B.x, B.z}, {C.x, C.z}, {D.x, D.z})) return true;
        }
        return false;
      };
      // runs of bridge legs joined at their points
      std::vector<int> parent(plan.nodes.size());
      for (size_t i = 0; i < parent.size(); ++i) parent[i] = int(i);
      auto root = [&](int i) {
        while (parent[size_t(i)] != i) i = parent[size_t(i)] = parent[size_t(parent[size_t(i)])];
        return i;
      };
      for (const streets::Seg& sg : plan.segs)
        if (sg.alive && sg.bridge) parent[size_t(root(sg.a))] = root(sg.b);
      std::map<int, float> deck, need;
      for (const streets::Seg& sg : plan.segs) {
        if (!sg.alive || !sg.bridge) continue;
        const streets::Node &A = plan.nodes[size_t(sg.a)], &B = plan.nodes[size_t(sg.b)];
        int r = root(sg.a);
        if (!deck.count(r)) deck[r] = -1e9f, need[r] = -1e9f;
        for (int n : {sg.a, sg.b})
          if (!low[size_t(n)]) deck[r] = std::max(deck[r], plan.nodes[size_t(n)].y);
        float ground = -1e9f;
        for (int k = 0; k <= 16; ++k) {
          double t = k / 16.0;
          ground = std::max(ground, groundY(g, A.x + (B.x - A.x) * t, A.z + (B.z - A.z) * t));
        }
        need[r] = std::max(need[r], ground + ((sg.osmBridge && crossesStreet(sg)) ? 6.0f : 1.0f));
      }
      int runs = 0;
      for (auto& [r, dk] : deck) {
        float top = std::max(dk, need[r]);
        ++runs;
        for (size_t i = 0; i < plan.nodes.size(); ++i)
          if (plan.nodes[i].alive && plan.nodes[i].exit < 0 && root(int(i)) == r) {
            // a run's point: the deck; a bank point on it keeps the deck too (the street beyond ramps up)
            bool onRun = false;
            for (const streets::Seg& sg : plan.segs)
              if (sg.alive && sg.bridge && (sg.a == int(i) || sg.b == int(i))) onRun = true;
            if (onRun) plan.nodes[i].y = top;
          }
      }
      if (std::getenv("REGS_DEBUG_OSM")) LOG_INFO("osm: %zu bridge runs", deck.size());
      (void)runs;
    }
    std::vector<CurveRoad> cs = streets::curves(plan);
    if (!d.tramLines.empty()) {  // street trams: a road curve with a tram line along most of it gets the tracks (ends bit 2)
      std::map<std::pair<int, int>, std::vector<std::pair<std::pair<double, double>, std::pair<double, double>>>> legs;  // 8 × 8 cell buckets
      for (auto& line : d.tramLines)
        for (size_t k = 0; k + 1 < line.size(); ++k) {
          std::pair<double, double> a2 = line[k], b2 = line[k + 1];
          for (int bz = int(std::floor(std::min(a2.second, b2.second) / 8)); bz <= int(std::floor(std::max(a2.second, b2.second) / 8)); ++bz)
            for (int bx = int(std::floor(std::min(a2.first, b2.first) / 8)); bx <= int(std::floor(std::max(a2.first, b2.first) / 8)); ++bx)
              legs[{bx, bz}].push_back({a2, b2});
        }
      int tracked = 0;
      for (CurveRoad& c : cs) {
        if (c.kind > 2 || c.bridge || (c.ends & 0x80)) continue;
        const double tol = std::max(1.5, curveProfile(c.kind).carriageway / g.cellSize + 0.8);
        int on = 0, all = 0;
        for (const CurveSample& sm : sampleCurve(c, 3.0f)) {
          double x = sm.p.x / g.cellSize + W * 0.5, z = sm.p.z / g.cellSize + L * 0.5;
          ++all;
          auto it = legs.find({int(std::floor(x / 8)), int(std::floor(z / 8))});
          if (it == legs.end()) continue;
          for (auto& [a2, b2] : it->second)
            if (segDist(x, z, a2, b2) <= tol) {
              ++on;
              break;
            }
        }
        if (all && on * 10 >= all * 6) {
          c.ends |= 0x4, ++tracked;
          if (std::getenv("REGS_DEBUG_OSM")) LOG_INFO("osm: tram tracks on a curve at world %.0f, %.0f", (c.x0 + c.x1) * 0.5, (c.z0 + c.z1) * 0.5);
        }
      }
      LOG_INFO("osm: %zu tram lines, %d street curves with tram tracks", d.tramLines.size(), tracked);
    }
    std::vector<LevelCrossing> crossings = cutLevelCrossings(cs);
    addCurvesBulk(vm, grid, a.timeline, cs);
    markCurveCells(g, cs, a.used);
    placeLevelCrossings(vm, grid, a, crossings);
    if (std::getenv("REGS_DEBUG_OSM")) {
      int w;
      float up = groundAbovePavement(g, cs, w);
      LOG_INFO("osm: ground above pavement: %.2f (curve %d)", up, w);
    }
    a.curvesMade = int(cs.size());
    if (const char* dr = std::getenv("REGS_DEBUG_OSM_RAILS")) debugRails(g, d, dr);
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
    if (a.next == 0) {  // the blocks of row houses first; their buildings are done then
      std::map<int, std::vector<const BuildingIn*>> blocks;
      for (const BuildingIn& b : d.buildings)
        if (b.row >= 0) blocks[b.row].push_back(&b);
      a.inRow.assign(d.buildings.size(), 0);
      a.yardSeeds.clear();
      int made = 0, houses = 0;
      for (auto& [id, members] : blocks) {
        int n = placeBlock(vm, grid, a, members, buildings, cache);
        houses += n, made += n > 0;
      }
      LOG_INFO("osm: %d of %zu blocks built as rows of city houses (%d houses)", made, blocks.size(), houses);
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
  if (a.stage == 4) {  // furnishing: OSM's street furniture, squares, car parks, street lamps, the yards
    setP(7, 1.0f);
    Furnish fu;
    if (ArrayObj* arr = vm.resolve(trees).array())
      for (const Value& v : arr->items) {
        std::string n = utf16ToUtf8(vm.toStr(v));
        if (!grid->templates.count(n)) continue;
        if (n.rfind("bush", 0) == 0) fu.bushes.push_back(n);
        else if (n == "tree1" || n == "tree2" || n == "tree3" || n == "street-tree") fu.trees.push_back(n);
      }
    if (fu.trees.empty()) fu.trees = fu.bushes;
    if (fu.bushes.empty()) fu.bushes = fu.trees;
    buildTemplates(vm, grid, buildings, trees);
    gridEditBegin(vm, grid);
    std::vector<std::pair<double, double>> lamps;
    placePoints(vm, grid, a, lamps);
    int squares = 0, bays = 0;
    for (const AreaIn& ar : d.areas)
      if (ar.kind != 1) decorateSquare(vm, grid, a, ar, fu), ++squares;
    for (const AreaIn& ar : d.areas)
      if (ar.kind == 1) bays += decorateParking(vm, grid, a, ar, 3000 - bays);
    LOG_INFO("osm: %d squares furnished, %d parking bays", squares, bays);
    if (!a.gridStreets) streetLamps(vm, grid, a, lamps);
    Painter painter{g};
    decorateYards(vm, grid, a, fu, painter);
    if (painter.x1 >= painter.x0) {  // the yards' pavement: on the timeline as a patch and the edges
      gridEditCellsChanged(grid, painter.x0, painter.z0, painter.x1, painter.z1, false);
      if (a.timeline) {
        const int pw = painter.x1 - painter.x0 + 1, pl = painter.z1 - painter.z0 + 1;
        std::vector<int16_t> hs(size_t(pw) * size_t(pl));
        std::vector<uint16_t> fs(hs.size());
        for (int z = 0; z < pl; ++z)
          for (int x = 0; x < pw; ++x) {
            size_t i = size_t(painter.z0 + z) * size_t(W) + size_t(painter.x0 + x);
            hs[size_t(z) * size_t(pw) + size_t(x)] = g.height[i];
            fs[size_t(z) * size_t(pw) + size_t(x)] = uint16_t(g.flags[i] & 0x203F);
          }
        timelineRecordPatch(vm, a.timeline, painter.x0, painter.z0, pw, pl, hs, fs);
        timelineRecordPaintEdges(vm, a.timeline, g, painter.x0, painter.z0, pw, pl);
      }
    }
    gridEditEnd(vm, grid);
    if (std::getenv("REGS_DEBUG_OSM")) {
      std::string all;
      for (auto& [n, c] : g_placedCount) all += n + " " + std::to_string(c) + ", ";
      LOG_INFO("osm: placed: %s", all.c_str());
    }
    g_placedCount.clear();
    a.stage = 5;
    a.next = 0;
    if (now() - t0 > budget) return false;
  }
  if (a.stage == 5) {  // trees
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
    a.stage = 6;
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
