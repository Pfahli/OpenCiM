// Passenger silhouettes from the game's citizen models (engine/passenger_atlas.h).
#include "engine/passenger_atlas.h"

#include <algorithm>
#include <cmath>
#include <regex>

#include "core/log.h"
#include "render/gpu.h"
#include "render/mesh.h"
#include "render/skeleton.h"
#include "vfs/vfs.h"

namespace regs::engine {

using namespace regs::render;

namespace {

// "50.0 / 25.0" or "2.0": the crop values as the character scripts write them
float number(const std::string& a, const std::string& b) {
  float x = float(std::atof(a.c_str()));
  if (!b.empty()) {
    float d = float(std::atof(b.c_str()));
    if (d != 0) x /= d;
  }
  return x;
}

struct Model {
  std::string object, skeleton, animation;
  float cropStart = 0, cropEnd = -1;
};

// the model a character script describes: its skeleton and the "idle" animation with its crop
bool readScript(const Vfs& vfs, const std::string& dir, const std::string& script, Model& m) {
  std::vector<uint8_t> bytes;
  if (!vfs.read(dir + script, bytes)) return false;
  std::string text(bytes.begin(), bytes.end());
  std::smatch s;
  static const std::regex skel(R"re(loadSkeleton\(\s*"([^"]+)")re");
  static const std::regex anim(R"re(loadAnimation\(\s*"idle"\s*,\s*"([^"]+)")re");
  static const std::regex crop(
      R"re(cropAnimation\(\s*"idle"\s*,\s*([0-9.]+)\s*(?:/\s*([0-9.]+))?\s*,\s*([0-9.]+)\s*(?:/\s*([0-9.]+))?)re");
  if (!std::regex_search(text, s, skel)) return false;
  m.skeleton = dir + s[1].str();
  if (!std::regex_search(text, s, anim)) return false;
  m.animation = dir + s[1].str();
  if (std::regex_search(text, s, crop)) {
    m.cropStart = number(s[1].str(), s[2].str());
    m.cropEnd = number(s[3].str(), s[4].str());
  }
  m.object = dir + script.substr(0, script.size() - 7) + ".object";
  return vfs.exists(m.object) && vfs.exists(m.skeleton) && vfs.exists(m.animation);
}

// one figure: the posed vertices, then per angle the triangles filled at 4×4 samples per texel
void rasterise(const MeshData& mesh, const std::vector<Vec3>& posed, int row, PassengerAtlas& out) {
  constexpr int S = 4;
  const int W = PassengerAtlas::kCellW * S, H = PassengerAtlas::kCellH * S;
  std::vector<uint8_t> hi(size_t(W) * size_t(H));
  for (int col = 0; col < PassengerAtlas::kCols; ++col) {
    std::fill(hi.begin(), hi.end(), uint8_t(0));
    float a = float(col) * 0.7853982f, ca = std::cos(a), sa = std::sin(a);
    // to hi-res texels: x across (the view looks along the turned z), y down from the top of the cell
    auto px = [&](const Vec3& p, float& u, float& v) {
      float x = p.x * ca + p.z * sa;
      u = (x / PassengerAtlas::kUnitsW + 0.5f) * float(W);
      v = (PassengerAtlas::kUnitsH - p.y) / PassengerAtlas::kUnitsH * float(H);
    };
    for (size_t k = 0; k + 2 < mesh.indices.size(); k += 3) {
      float u[3], v[3];
      for (int j = 0; j < 3; ++j) px(posed[mesh.indices[k + size_t(j)]], u[j], v[j]);
      float det = (u[1] - u[0]) * (v[2] - v[0]) - (u[2] - u[0]) * (v[1] - v[0]);
      if (std::fabs(det) < 1e-6f) continue;
      int x0 = std::max(0, int(std::floor(std::min({u[0], u[1], u[2]})))), x1 = std::min(W - 1, int(std::ceil(std::max({u[0], u[1], u[2]}))));
      int y0 = std::max(0, int(std::floor(std::min({v[0], v[1], v[2]})))), y1 = std::min(H - 1, int(std::ceil(std::max({v[0], v[1], v[2]}))));
      for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
          float qx = float(x) + 0.5f, qy = float(y) + 0.5f;
          float l1 = ((qx - u[0]) * (v[2] - v[0]) - (u[2] - u[0]) * (qy - v[0])) / det;
          float l2 = ((u[1] - u[0]) * (qy - v[0]) - (qx - u[0]) * (v[1] - v[0])) / det;
          if (l1 < 0 || l2 < 0 || l1 + l2 > 1) continue;
          hi[size_t(y) * size_t(W) + size_t(x)] = 1;
        }
    }
    // down to coverage
    for (int y = 0; y < PassengerAtlas::kCellH; ++y)
      for (int x = 0; x < PassengerAtlas::kCellW; ++x) {
        int sum = 0;
        for (int j = 0; j < S; ++j)
          for (int i = 0; i < S; ++i) sum += hi[size_t(y * S + j) * size_t(W) + size_t(x * S + i)];
        size_t at = size_t(row * PassengerAtlas::kCellH + y) * size_t(out.width()) + size_t(col * PassengerAtlas::kCellW + x);
        out.pixels[at] = uint8_t(sum * 255 / (S * S));
      }
  }
}

}  // namespace

bool buildPassengerAtlas(const Vfs& vfs, PassengerAtlas& out) {
  out = PassengerAtlas{};
  auto read = [&](const std::string& p, std::vector<uint8_t>& d) { return vfs.read(p, d); };
  std::vector<Model> models;
  for (const std::string& era : vfs.listDirs("metro/objects/characters"))
    for (const std::string& type : vfs.listDirs("metro/objects/characters/" + era)) {
      std::string dir = "metro/objects/characters/" + era + "/" + type + "/";
      for (const std::string& f : vfs.listFiles(dir, ".script")) {
        Model m;
        if (readScript(vfs, dir, f, m)) {
          models.push_back(m);
          break;  // one model per type
        }
      }
    }
  std::vector<std::pair<Model, std::vector<Vec3>>> posed;
  std::vector<MeshData> meshes;
  for (const Model& m : models) {
    ObjectData od;
    SkeletonData sk;
    AnimationData an;
    std::string err;
    if (!loadObject(m.object, read, od, &err) || !loadSkeleton(m.skeleton, read, sk, &err) ||
        !loadAnimation(m.animation, read, an, &err) || od.mesh.skinBones.empty()) {
      LOG_WARN("passenger atlas: %s skipped (%s)", m.object.c_str(), err.c_str());
      continue;
    }
    // the middle of the idle crop
    float end = m.cropEnd > m.cropStart ? m.cropEnd : an.duration;
    float time = m.cropStart + 0.5f * (end - m.cropStart);
    size_t n = sk.bones.size();
    std::vector<Vec3> t(n), s(n);
    std::vector<Quat> q(n);
    for (size_t i = 0; i < n; ++i) t[i] = sk.bones[i].t, s[i] = sk.bones[i].s, q[i] = sk.bones[i].q;
    for (const AnimTrack& tr : an.tracks)
      if (int b = sk.find(tr.bone); b >= 0) sampleTrack(tr, an.duration, time, false, t[size_t(b)], s[size_t(b)], q[size_t(b)]);
    std::vector<Mat4> pal;
    skinPalette(sk, t, q, s, pal);
    const MeshData& mesh = od.mesh;
    std::vector<Vec3> pos(mesh.vertexCount());
    for (size_t v = 0; v < mesh.vertexCount(); ++v) {
      Vec3 p{mesh.positions[v * 3], mesh.positions[v * 3 + 1], mesh.positions[v * 3 + 2]}, o{};
      for (int k = 0; k < 4; ++k) {
        float w = mesh.skinWeights[v * 4 + size_t(k)];
        uint16_t b = mesh.skinBones[v * 4 + size_t(k)];
        if (w > 0 && b < pal.size()) o = o + transformPoint(pal[b], p) * w;
      }
      pos[v] = o;
    }
    posed.emplace_back(m, std::move(pos));
    meshes.push_back(std::move(od.mesh));
  }
  if (posed.empty()) return false;
  out.rows = int(posed.size());
  out.pixels.assign(size_t(out.width()) * size_t(out.height()), 0);
  for (size_t r = 0; r < posed.size(); ++r) {
    rasterise(meshes[r], posed[r].second, int(r), out);
    out.models.push_back(posed[r].first.object);
  }
  return true;
}

uint32_t passengerAtlasTexture(const Vfs& vfs, float info[4]) {
  static bool tried = false;
  static uint32_t tex = 0;
  static int rows = 0;
  if (!tried && gpu::hasDevice()) {
    tried = true;
    PassengerAtlas a;
    if (buildPassengerAtlas(vfs, a)) {
      using namespace render::gpu;
      tex = dev().createTexture(TexKind::Tex2D, 3);
      SamplerDesc sd;
      sd.minFilter = Filter::LinearMipLinear;
      sd.wrapS = sd.wrapT = Wrap::ClampToEdge;
      sd.maxLevel = 2;
      dev().setSampler(tex, sd);
      int w = a.width(), h = a.height();
      std::vector<uint8_t> level = a.pixels;
      for (int l = 0; l < 3; ++l) {
        dev().texImage(tex, 0, l, Format::R8, w, h, Pixels::R8, level.data());
        if (l == 2) break;
        std::vector<uint8_t> next(size_t(w / 2) * size_t(h / 2));
        for (int y = 0; y < h / 2; ++y)
          for (int x = 0; x < w / 2; ++x) {
            int sum = level[size_t(2 * y) * size_t(w) + size_t(2 * x)] + level[size_t(2 * y) * size_t(w) + size_t(2 * x + 1)] +
                      level[size_t(2 * y + 1) * size_t(w) + size_t(2 * x)] + level[size_t(2 * y + 1) * size_t(w) + size_t(2 * x + 1)];
            next[size_t(y) * size_t(w / 2) + size_t(x)] = uint8_t((sum + 2) / 4);
          }
        level.swap(next);
        w /= 2, h /= 2;
      }
      rows = a.rows;
      LOG_INFO("passenger atlas: %d citizen models x %d angles, %dx%d texels", a.rows, PassengerAtlas::kCols, a.width(), a.height());
    } else {
      LOG_WARN("passenger atlas: no citizen models found, the windows keep the plain figures");
    }
  }
  info[0] = tex ? float(rows) : 0.0f;
  info[1] = float(PassengerAtlas::kCols);
  info[2] = PassengerAtlas::kUnitsH;
  info[3] = PassengerAtlas::kUnitsW;
  return tex;
}

}  // namespace regs::engine
