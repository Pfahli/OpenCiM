// regs-tool: developer commands for the ReGS port.
//   regs-tool vfs <gameDir> stats|verify|list [prefix]|cat <path> [out]
//   regs-tool parse-all <gameDir> [maxErrors]   parse every script-source file in the VFS
//   regs-tool parse <file>                      dump the AST of one file on disk
#include <chrono>
#include <cmath>
#include <cstdio>
#include <set>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <algorithm>

#include <thread>

#include "audio/audio.h"
#include "core/log.h"
#include "core/text.h"
#include "engine/api.h"
#include "engine/effects.h"
#include "engine/engine.h"
#include "engine/host.h"
#include "engine/night_lights.h"
#include "engine/shaders.h"
#include "platform/window.h"
#include "render/gl.h"
#include "render/image.h"
#include "render/mesh.h"
#include "render/renderer3d.h"
#include "render/skeleton.h"
#include "script/compiler.h"
#include "script/parser.h"
#include "vfs/vfs.h"

using namespace regs;
namespace fs = std::filesystem;

int cmdCut(int argc, char** argv);      // mod_assets.cpp
int cmdSoftDot(int argc, char** argv);  // mod_assets.cpp
int cmdLeaf(int argc, char** argv);     // mod_assets.cpp

namespace {

fs::path widen(const char* s) {
  auto w = utf8ToUtf16(s);
  return fs::path(std::wstring(w.begin(), w.end()));
}

bool isScriptSource(const std::string& path) {
  static const char* exts[] = {".script", ".scenario", ".strings", ".campaign", ".keys", ".shader"};
  for (auto e : exts) {
    size_t n = std::strlen(e);
    if (path.size() > n && path.compare(path.size() - n, n, e) == 0) return true;
  }
  return false;
}

int cmdVfs(int argc, char** argv) {
  if (argc < 4) return 2;
  Vfs vfs;
  vfs.setInstallRoot(widen(argv[2]));
  vfs.setCommunityContent(true);  // the file-system and parser checks cover the whole install
  auto t0 = std::chrono::steady_clock::now();
  vfs.rescan();
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  std::string sub = argv[3];
  if (sub == "stats") {
    size_t entries = 0, loose = 0, fromArchives = 0;
    for (auto& a : vfs.archives()) entries += a.entries.size();
    for (auto& [k, r] : vfs.records()) (r.kind == Vfs::Kind::Loose ? loose : fromArchives)++;
    std::printf("scan %lld ms: %zu archives, %zu archive entries, %zu records (%zu from archives, %zu loose)\n",
                (long long)ms, vfs.archives().size(), entries, vfs.records().size(), fromArchives, loose);
    for (auto& a : vfs.archives())
      std::printf("  %-60s %6zu entries  mount=%s\n", a.file.filename().string().c_str(), a.entries.size(),
                  a.mountPath ? ("abs:" + *a.mountPath).c_str() : ("prefix:" + a.parentPrefix.value_or("")).c_str());
    return 0;
  }
  if (sub == "verify") {
    size_t ok = 0, bad = 0;
    uint64_t bytes = 0;
    std::vector<uint8_t> buf;
    for (auto& a : vfs.archives())
      for (auto& e : a.entries) {
        std::string err;
        if (a.read(e, buf, &err)) {
          ++ok;
          bytes += buf.size();
          if (buf.size() >= 4 && std::memcmp(buf.data(), &buf[0], 4) != 0) ++bad;
        } else {
          ++bad;
          std::printf("FAIL %s:%s: %s\n", a.file.filename().string().c_str(), e.name.c_str(), err.c_str());
        }
      }
    std::printf("verified %zu entries (%llu bytes), %zu failures\n", ok, (unsigned long long)bytes, bad);
    return bad ? 1 : 0;
  }
  if (sub == "list") {
    std::string prefix = argc > 4 ? argv[4] : "";
    for (auto& [k, r] : vfs.records())
      if (k.compare(0, prefix.size(), prefix) == 0) std::printf("%s\n", k.c_str());
    return 0;
  }
  if (sub == "cat" && argc > 4) {
    std::vector<uint8_t> data;
    std::string err;
    if (!vfs.read(argv[4], data, &err)) {
      std::fprintf(stderr, "%s: %s\n", argv[4], err.c_str());
      return 1;
    }
    if (argc > 5) {
      FILE* f = _wfopen(widen(argv[5]).wstring().c_str(), L"wb");
      if (!f) return 1;
      std::fwrite(data.data(), 1, data.size(), f);
      std::fclose(f);
    } else {
      std::fwrite(data.data(), 1, data.size(), stdout);
    }
    return 0;
  }
  return 2;
}

int cmdParseAll(int argc, char** argv) {
  if (argc < 3) return 2;
  int maxShow = argc > 3 ? std::atoi(argv[3]) : 40;
  Vfs vfs;
  vfs.setInstallRoot(widen(argv[2]));
  vfs.setCommunityContent(true);  // the file-system and parser checks cover the whole install
  vfs.rescan();
  size_t files = 0, failed = 0, bytes = 0, bytecode = 0;
  std::map<std::string, std::vector<std::string>> byMessage;
  std::vector<uint8_t> data;
  auto t0 = std::chrono::steady_clock::now();
  for (auto& [path, rec] : vfs.records()) {
    if (!isScriptSource(path)) continue;
    ++files;
    if (!vfs.read(path, data)) {
      ++failed;
      byMessage["<read error>"].push_back(path);
      continue;
    }
    bytes += data.size();
    auto src = decodeTextFile(data);
    auto res = script::parse(src);
    if (!res.program) {
      ++failed;
      auto colon = res.error.find(": ");
      std::string msg = colon == std::string::npos ? res.error : res.error.substr(colon + 2);
      byMessage[msg].push_back(path + ":" + std::to_string(res.line) + ":" + std::to_string(res.col));
      continue;
    }
    auto cr = script::compile(res.program.get(), utf8ToUtf16(path));
    if (!cr.code) {
      ++failed;
      auto colon = cr.error.find(": ");
      std::string msg = "compile: " + (colon == std::string::npos ? cr.error : cr.error.substr(colon + 2));
      byMessage[msg].push_back(path + ":" + std::to_string(cr.line));
    } else {
      bytecode += cr.code->bc.size();
    }
  }
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  int shown = 0;
  for (auto& [msg, list] : byMessage) {
    std::printf("%5zu  %s\n", list.size(), msg.c_str());
    for (size_t i = 0; i < list.size() && i < 3 && shown < maxShow; ++i, ++shown) std::printf("         %s\n", list[i].c_str());
  }
  std::printf("parsed and compiled %zu files (%zu bytes source, %zu bytes bytecode) in %lld ms: %zu failed\n", files,
              bytes, bytecode, (long long)ms, failed);
  return failed ? 1 : 0;
}

int cmdParse(int argc, char** argv) {
  if (argc < 3) return 2;
  FILE* f = _wfopen(widen(argv[2]).wstring().c_str(), L"rb");
  if (!f) return 1;
  std::vector<uint8_t> data;
  uint8_t buf[65536];
  size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) data.insert(data.end(), buf, buf + n);
  std::fclose(f);
  auto res = script::parse(decodeTextFile(data));
  if (!res.program) {
    std::printf("error: %s\n", res.error.c_str());
    return 1;
  }
  std::fputs(script::dumpAst(res.program.get()).c_str(), stdout);
  return 0;
}

// run <gameDir> [frames] [sandbox]: boot the game headless with userdata in a sandbox folder
int cmdRun(int argc, char** argv) {
  if (argc < 3) return 2;
  int frames = argc > 3 ? std::atoi(argv[3]) : 60;
  fs::path sandbox = argc > 4 ? widen(argv[4]) : fs::current_path() / "sandbox";
  fs::create_directories(sandbox);
  engine::Engine eng(widen(argv[2]), sandbox);
  eng.echo = [](const std::string& s) { std::printf("[script] %s\n", s.c_str()); };
  eng.errors = [](const std::string& s) { std::printf("[error] %s\n", s.c_str()); };
  auto t0 = std::chrono::steady_clock::now();
  bool ok = eng.init(u"metro/scripts/init.script");
  for (int i = 0; i < frames && !engine::host().quit; ++i) eng.frame(1.0f / 60.0f);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  std::printf("init %s; %d frames in %lld ms; %llu instructions; %zu live threads\n", ok ? "ok" : "FAILED", frames,
              (long long)ms, (unsigned long long)eng.vm().instructions, eng.vm().threadCount());
  for (const char16_t* sm : {u"system", u"main"}) {
    script::Value* v = eng.main()->findVar(sm);
    std::printf("state machine $%s: %s\n", utf16ToUtf8(sm).c_str(),
                v && v->object() ? utf16ToUtf8(engine::currentStateName(v->object())).c_str() : "<missing>");
  }
  std::vector<std::pair<int, std::string>> miss;
  for (auto& [k, n] : eng.vm().missing) miss.emplace_back(n, k);
  std::sort(miss.begin(), miss.end(), [](auto& a, auto& b) { return a.first > b.first; });
  std::printf("%zu distinct unimplemented natives / unresolved names:\n", miss.size());
  for (size_t i = 0; i < miss.size() && i < 60; ++i) std::printf("  %6d  %s\n", miss[i].first, miss[i].second.c_str());
  return ok ? 0 : 1;
}

// shader <gameDir> <path.shader|""> <matFlags> <meshFlags> <envFlags>: generate one combiner variant and print its
// GLSL 1.10 (as the engine builds it) after init.script set up MAIN
int cmdShader(int argc, char** argv) {
  if (argc < 7) return 2;
  fs::path sandbox = fs::temp_directory_path() / "regs-shader-tool";
  fs::create_directories(sandbox);
  engine::Engine eng(widen(argv[2]), sandbox);
  eng.init(u"metro/scripts/init.script");
  auto num = [](const char* s) { return uint32_t(std::strtoul(s, nullptr, 0)); };
  engine::ShaderCombiner* c = engine::getCombiner(eng.vm(), utf8ToUtf16(argv[3]));
  if (!c) return 1;
  engine::ShaderVariant* v = engine::combinerVariant(eng.vm(), c, num(argv[4]), num(argv[5]), num(argv[6]));
  std::printf("// attributes 0x%x textures 0x%x parameters 0x%x%s\n// ---- vertex ----\n%s// ---- fragment ----\n%s",
              v->attributes, v->textures, v->parameters, v->failed ? " FAILED" : "", v->vertex.c_str(), v->fragment.c_str());
  return v->failed ? 1 : 0;
}

// shaders <gameDir>: generate and compile shader variants on a hidden OpenGL context: every .shader file with all of
// its material bits and with none, and the default shader for the flags of every material of every shipped .object
// (with and without skinning, in the normal and the shadow passes)
int cmdShaders(int argc, char** argv) {
  if (argc < 3) return 2;
  fs::path sandbox = fs::temp_directory_path() / "regs-shader-tool";
  fs::create_directories(sandbox);
  engine::Engine eng(widen(argv[2]), sandbox);
  eng.errors = [](const std::string& s) { std::printf("[error] %s\n", s.c_str()); };
  eng.init(u"metro/scripts/init.script");
  auto win = platform::createWindow();
  platform::WindowDesc desc;
  desc.width = desc.height = 64;
  desc.visible = false;
  std::string err;
  if (!win->create(desc, &err)) {
    std::fprintf(stderr, "no OpenGL context: %s\n", err.c_str());
    return 1;
  }
  auto proc = [](const char* n, void* u) { return static_cast<platform::Window*>(u)->glProc(n); };
  if (const char* missing = gl::load(proc, win.get())) {
    std::fprintf(stderr, "missing %s\n", missing);
    return 1;
  }
  render::Renderer3D r3d;
  if (!r3d.init(&err)) {
    std::fprintf(stderr, "renderer: %s\n", err.c_str());
    return 1;
  }
  int ok = 0, failed = 0;
  std::map<std::string, std::pair<int, int>> perFile;  // failed, tested
  auto test = [&](engine::ShaderCombiner* c, uint32_t mat, uint32_t mesh, uint32_t env) {
    auto& pf = perFile[utf16ToUtf8(c->path)];
    ++pf.second;
    engine::ShaderVariant* v = engine::combinerVariant(eng.vm(), c, mat, mesh, env);
    std::string e;
    if (!v->failed && !v->program) v->program = r3d.buildProgram(v->vertex, v->fragment, &e);
    if (v->failed || !v->program) {
      if (!v->failed) v->failed = true;
      ++pf.first;
      if (++failed <= 3)
        std::printf("FAIL %s mat 0x%x mesh 0x%x env 0x%x: %s\n", utf16ToUtf8(c->path).c_str(), mat, mesh, env,
                    e.empty() ? "generation failed" : e.substr(0, 600).c_str());
    } else {
      ++ok;
    }
  };
  // material flag sets that the shipped meshes actually produce
  std::set<uint32_t> flagsets;
  int objects = 0;
  for (auto& [path, rec] : eng.vfs().records()) {
    if (path.size() < 7 || path.compare(path.size() - 7, 7, ".object") != 0) continue;
    render::ObjectData od;
    if (!render::loadObject(path, [&](const std::string& p, std::vector<uint8_t>& o) { return eng.vfs().read(p, o); }, od, nullptr))
      continue;
    ++objects;
    for (const auto& m : od.materials) flagsets.insert(m.flags);
  }
  int files = 0;
  for (auto& [path, rec] : eng.vfs().records()) {
    if (path.size() < 7 || path.compare(path.size() - 7, 7, ".shader") != 0 || path.rfind("userdata/", 0) == 0) continue;
    ++files;
    engine::ShaderCombiner* c = engine::getCombiner(eng.vm(), utf8ToUtf16(path));
    if (!c) {
      std::printf("FAIL %s: cannot load\n", path.c_str());
      ++failed;
      continue;
    }
    // every shader with no material bits and with each flag set that shipped materials produce (not "all bits":
    // that combination overruns the default shader's texture-coordinate array and never occurs)
    test(c, 0, 0, 0x1000);
    for (uint32_t f : flagsets) test(c, f, 0, 0x1000);
  }
  engine::ShaderCombiner* def = engine::getCombiner(eng.vm(), u"");
  if (def)
    for (uint32_t f : flagsets)
      for (uint32_t mesh : {0u, 0x100u})
        for (uint32_t env : {0x1000u, 0x1010u, 0x4u, 0xAu}) test(def, f, mesh, env);
  size_t variants = 0;
  for (auto& [f, n] : perFile)
    if (n.first) std::printf("  %-48s %d of %d variants failed\n", f.c_str(), n.first, n.second);
  std::printf("%d shader files, %d objects, %zu material flag sets: %d programs compiled, %d failed\n", files, objects,
              flagsets.size(), ok, failed);
  (void)variants;
  return failed ? 1 : 0;
}

// script <file>: run a script from disk (its folder's parent is the VFS root) for one second of frames, then fire
// the events vm_basics.script binds and print the state the asynchronous checks depend on.
int cmdScript(int argc, char** argv) {
  if (argc < 3) return 2;
  fs::path file = fs::absolute(widen(argv[2]));
  fs::path root = file.parent_path().parent_path();
  engine::host().communityAddons = true;  // the test folder is loose files, not a game install with archives
  engine::Engine eng(root, root / "build" / "sandbox");
  eng.echo = [](const std::string& s) { std::printf("[script] %s\n", s.c_str()); };
  eng.errors = [](const std::string& s) { std::printf("[error] %s\n", s.c_str()); };
  auto rel = fs::relative(file, root).generic_wstring();
  script::Value obj = eng.vm().load(std::u16string(rel.begin(), rel.end()));
  script::Object* o = obj.object();
  if (!o) return 1;
  for (int i = 0; i < 60; ++i) eng.frame(1.0f / 60.0f);
  if (script::Value* mod = o->findVar(u"mod"); mod && mod->object()) {
    script::Value esc = script::Value::string("Esc"), enter = script::Value::string("Enter");
    eng.vm().fireEvent(mod->object(), u"onAddChild", {script::Value::integer(5)});
    eng.vm().fireEvent(mod->object(), u"keyDown", {}, &esc);
    eng.vm().fireEvent(mod->object(), u"keyDown", {}, &enter);
  }
  auto show = [&](const char16_t* n) {
    script::Value* v = o->findVar(n);
    std::printf("  $%s = %s\n", utf16ToUtf8(n).c_str(), v ? v->toUtf8().c_str() : "<unset>");
  };
  std::printf("after 1 s of frames (expect ticks 10 or 11, done true, fired 1105, grouped false):\n");
  show(u"grouped");
  show(u"ticks");
  show(u"done");
  show(u"fired");
  return 0;
}

// skintest <gameDir> <object> <skeleton> <animation> [cropStart length]: skins the mesh's vertices on the CPU with
// render/skeleton (docs/renderer/skinning.md) at the bind pose and at 8 times of the clip; prints how far the bind
// palette is from identity and the posed bounding box per time
int cmdSkinTest(int argc, char** argv) {
  if (argc < 6) return 2;
  Vfs vfs;
  vfs.setInstallRoot(widen(argv[2]));
  vfs.rescan();
  auto read = [&](const std::string& p, std::vector<uint8_t>& out) { return vfs.read(p, out); };
  render::ObjectData od;
  render::SkeletonData sk;
  render::AnimationData an;
  std::string err;
  if (!render::loadObject(argv[3], read, od, &err) || !render::loadSkeleton(argv[4], read, sk, &err) ||
      !render::loadAnimation(argv[5], read, an, &err)) {
    std::printf("load failed: %s\n", err.c_str());
    return 1;
  }
  const render::MeshData& m = od.mesh;
  float crop = argc > 6 ? float(std::atof(argv[6])) : 0.0f, len = argc > 7 ? float(std::atof(argv[7])) : an.duration;
  std::printf("%zu vertices, skin %s, %zu bones, %zu tracks, duration %.3f s\n", m.vertexCount(),
              m.skinBones.empty() ? "no" : "yes", sk.bones.size(), an.tracks.size(), an.duration);
  std::vector<int> trackOf(sk.bones.size(), -1);
  int bound = 0;
  for (size_t t = 0; t < an.tracks.size(); ++t)
    if (int b = sk.find(an.tracks[t].bone); b >= 0 && trackOf[size_t(b)] < 0) trackOf[size_t(b)] = int(t), ++bound;
  std::printf("tracks bound to bones: %d\n", bound);
  auto pose = [&](float time, std::vector<render::Mat4>& pal) {
    std::vector<render::Vec3> t(sk.bones.size()), s(sk.bones.size());
    std::vector<render::Quat> q(sk.bones.size());
    for (size_t i = 0; i < sk.bones.size(); ++i) {
      t[i] = sk.bones[i].t, s[i] = sk.bones[i].s, q[i] = sk.bones[i].q;
      if (time >= 0 && trackOf[i] >= 0) render::sampleTrack(an.tracks[size_t(trackOf[i])], an.duration, time, false, t[i], s[i], q[i]);
    }
    render::skinPalette(sk, t, q, s, pal);
  };
  auto box = [&](const std::vector<render::Mat4>& pal) {
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    for (size_t v = 0; v < m.vertexCount(); ++v) {
      render::Vec3 p{m.positions[v * 3], m.positions[v * 3 + 1], m.positions[v * 3 + 2]}, o{};
      for (int k = 0; k < 4 && !m.skinBones.empty(); ++k) {
        float w = m.skinWeights[v * 4 + size_t(k)];
        if (w > 0) o = o + render::transformPoint(pal[m.skinBones[v * 4 + size_t(k)]], p) * w;
      }
      for (int a = 0; a < 3; ++a) lo[a] = std::min(lo[a], (&o.x)[a]), hi[a] = std::max(hi[a], (&o.x)[a]);
    }
    std::printf("x %.2f..%.2f  y %.2f..%.2f  z %.2f..%.2f\n", lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
  };
  std::vector<render::Mat4> pal;
  pose(-1, pal);
  float dev = 0;
  for (const auto& mtx : pal)
    for (int i = 0; i < 16; ++i) dev = std::max(dev, std::fabs(mtx.m[i] - ((i % 5) == 0 ? 1.0f : 0.0f)));
  std::printf("bind palette: max |skin - identity| = %.2e; bind box: ", dev);
  box(pal);
  for (int i = 0; i < 8; ++i) {
    float time = crop + len * float(i) / 8.0f;
    pose(time, pal);
    std::printf("t %.3f: ", time);
    box(pal);
  }
  return 0;
}

// shadowtest: the light-camera fit on the worked example of docs/renderer/shadows.md §3.7, compared with its numbers
int cmdShadowTest(int, char**) {
  engine::ShadowFitIn in;
  in.eye = {-0.0984f, 113.186f, 98.433f};
  in.target = {0, 0, 0};
  in.up = {0.000755f, 0.656219f, -0.754571f};
  in.fov = 40;
  in.aspect = 16.0f / 9.0f;
  in.far = 310;
  in.clipYPlane = true;
  in.yPlane = -10;
  in.safeMargin = 0.4f;
  in.light = {9912.35f, 14265.0f, 9912.35f};
  in.mapWidth = in.mapHeight = 2048;
  engine::ShadowFitOut o;
  bool ok = engine::fitShadow(in, o);
  int bad = 0;
  auto check = [&](const char* what, float got, float want, float tol) {
    bool good = std::fabs(got - want) <= tol;
    bad += good ? 0 : 1;
    std::printf("%-22s %14.6f  want %14.6f  %s\n", what, got, want, good ? "ok" : "DIFF");
  };
  check("Z.x", o.axisZ.x, -0.493895f, 1e-4f);
  check("Z.y", o.axisZ.y, -0.713371f, 1e-4f);
  check("Z.z", o.axisZ.z, -0.497162f, 1e-4f);
  check("X(up).y", o.axisX.y, -0.571767f, 1e-4f);
  check("X(up).z", o.axisX.z, 0.820416f, 1e-4f);
  check("fov", o.lightFov, 1.150648f, 1e-3f);
  check("near", o.lightNear, 19742.60f, 0.5f);
  check("far", o.lightFar, 20346.48f, 0.5f);
  const float wantP[4][2] = {{-0.6667f, -0.3579f}, {-0.0981f, 0.3579f}, {0.6667f, -0.3565f}, {0.5961f, 0.3559f}};
  for (int i = 0; i < 4; ++i) {
    char n[16];
    std::snprintf(n, sizeof n, "P%d.x", i);
    check(n, o.projected[i][0], wantP[i][0], 2e-3f);
    std::snprintf(n, sizeof n, "P%d.y", i);
    check(n, o.projected[i][1], wantP[i][1], 2e-3f);
  }
  check("E.x", o.projectedEye[0], 0.5684f, 2e-3f);
  check("E.y", o.projectedEye[1], 0.2005f, 2e-3f);
  check("P2'.x", o.warpedCorners[2][0], 1.0667f, 2e-3f);
  check("P3'.x", o.warpedCorners[3][0], 0.9159f, 2e-3f);
  const float wantWarp[16] = {-1.15423f, 0.002435f, 0, 0.000634f, 0.336292f, -2.23167f, 0, -0.580840f,
                              0, 0, 1, 0, 0.350898f, 0.202832f, 0, 0.792519f};
  for (int i = 0; i < 16; ++i) {
    char n[16];
    std::snprintf(n, sizeof n, "warp[%d]", i);
    check(n, o.warp.m[i], wantWarp[i], 2e-3f * std::max(1.0f, std::fabs(wantWarp[i])));
  }
  check("params.x", o.params[0], 0.000244141f, 1e-7f);
  check("params.y", o.params[1], -20346.5f, 0.5f);
  check("params.w", o.params[3], 3.05197e-6f, 1e-9f);
  std::printf("%s, %d differences\n", ok ? "fit ok" : "fit failed", bad);
  return bad == 0 && ok ? 0 : 1;
}

// regs-tool sound <gameDir> <path> [seconds] [stream] [layout=<name>] [at=x,y,z] [capture=<file.wav>]: decode a sound
// from the VFS, print its format, play it for a few seconds (the audio layer's smoke test). at= plays it as a 3D voice
// (min 10, max 100) with the listener at the origin looking down −Z; layout= picks a speaker layout
// (audio_speakers() names); capture= also writes the mix to a WAV file.
int cmdSound(int argc, char** argv) {
  if (argc < 4) return 2;
  Vfs vfs;
  vfs.setInstallRoot(widen(argv[2]));
  vfs.rescan();
  std::vector<uint8_t> data;
  std::string err;
  if (!vfs.read(argv[3], data, &err)) {
    std::printf("not found: %s (%s)\n", argv[3], err.c_str());
    return 1;
  }
  double seconds = 3.0;
  bool stream = false, spatial = false;
  float at[3] = {0, 0, 0};
  audio::InitOptions io;
  for (int i = 4; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "stream") stream = true;
    else if (a.rfind("layout=", 0) == 0) io.layout = audio::layoutFromName(a.substr(7));
    else if (a.rfind("capture=", 0) == 0) io.capture = a.substr(8);
    else if (a.rfind("at=", 0) == 0) spatial = std::sscanf(a.c_str() + 3, "%f,%f,%f", &at[0], &at[1], &at[2]) == 3;
    else seconds = std::atof(a.c_str());
  }
  auto t0 = std::chrono::steady_clock::now();
  audio::ClipRef clip = audio::loadClip(std::move(data), stream, &err);
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  if (!clip) {
    std::printf("decode failed: %s\n", err.c_str());
    return 1;
  }
  audio::ClipInfo ci = audio::clipInfo(clip);
  std::printf("%s: %u ch, %u Hz, %llu frames (%.2f s), %s, decoded in %lld ms\n", argv[3], ci.channels, ci.sampleRate,
              (unsigned long long)ci.frames, ci.sampleRate ? double(ci.frames) / ci.sampleRate : 0.0,
              ci.stream ? "stream" : "pcm", (long long)ms);
  if (!audio::init(io)) return 1;
  std::printf("output: %s\n", audio::describeOutput().c_str());
  const float origin[3] = {0, 0, 0}, fwd[3] = {0, 0, -1}, up[3] = {0, 1, 0};
  audio::setListener(origin, fwd, up, origin);
  audio::VoiceParams p;
  p.loop = !stream && spatial;  // a short effect keeps going for the measurement
  p.spatial = spatial;
  for (int k = 0; k < 3; ++k) p.pos[k] = at[k];
  int v = audio::play(clip, p);
  std::printf("voice %d\n", v);
  auto start = std::chrono::steady_clock::now();
  while (audio::isPlaying(v) &&
         std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() < seconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    audio::update();
  }
  audio::Stats stats = audio::takeStats();
  std::printf("played %.2f s, %s; RMS per channel:", std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count(),
              audio::isPlaying(v) ? "still playing (cut)" : "finished");
  for (uint32_t c = 0; c < stats.channels && c < 8; ++c) std::printf(" %.4f", stats.rms[c]);
  std::printf("\n");
  audio::shutdown();
  return v > 0 ? 0 : 1;
}

// texpng <gameDir> <vfs path> <out.png> [level]: one level of a DDS texture as a PNG
// regs-tool materials <gameDir>: how many textures the game's materials have (every .material and .object in the
// archives, DLCs too): the texture units the default shader needs (render/renderer3d.h kMaterialUnits)
int cmdMaterials(int argc, char** argv) {
  if (argc < 3) return 2;
  Vfs vfs;
  vfs.setInstallRoot(widen(argv[2]));
  vfs.rescan();
  auto read = [&](const std::string& p, std::vector<uint8_t>& out) { return vfs.read(p, out); };
  std::map<size_t, size_t> histogram;
  size_t files = 0;
  for (const auto& [path, rec] : vfs.records()) {
    std::vector<render::MaterialDef> defs;
    if (path.size() > 9 && path.compare(path.size() - 9, 9, ".material") == 0) {
      render::MaterialDef def;
      if (render::loadMaterialFile(path, read, def)) defs.push_back(def);
    } else if (path.size() > 7 && path.compare(path.size() - 7, 7, ".object") == 0) {
      render::ObjectData obj;
      std::string err;
      if (render::loadObject(path, read, obj, &err)) defs = obj.materials;
    } else {
      continue;
    }
    ++files;
    for (const render::MaterialDef& d : defs) {
      ++histogram[d.textures.size()];
      if (d.textures.size() >= 5) {
        std::printf("%zu textures: %s / %s:", d.textures.size(), path.c_str(), d.name.c_str());
        for (const auto& t : d.textures) std::printf(" %s", t.c_str());
        std::printf("\n");
      }
    }
  }
  std::printf("%zu files\n", files);
  for (const auto& [n, c] : histogram) std::printf("%zu textures: %zu materials\n", n, c);
  return 0;
}

int cmdTexPng(int argc, char** argv) {
  if (argc < 5) return 2;
  Vfs vfs;
  vfs.setInstallRoot(widen(argv[2]));
  vfs.rescan();
  std::vector<uint8_t> data;
  std::string err;
  if (!vfs.read(argv[3], data, &err)) {
    std::fprintf(stderr, "%s: %s\n", argv[3], err.c_str());
    return 1;
  }
  render::Image img;
  std::vector<uint8_t> rgba;
  int w = 0, h = 0;
  if (!render::parseDds(data, img, &err) || !render::decodeRgba(img, argc > 5 ? std::atoi(argv[5]) : 0, rgba, w, h)) {
    std::fprintf(stderr, "%s: cannot decode (%s)\n", argv[3], err.c_str());
    return 1;
  }
  std::vector<uint8_t> png = render::encodePng(rgba.data(), w, h);
  FILE* f = _wfopen(widen(argv[4]).wstring().c_str(), L"wb");
  if (!f) return 1;
  std::fwrite(png.data(), 1, png.size(), f);
  std::fclose(f);
  std::printf("%s: %dx%d format %d, %d levels\n", argv[3], w, h, int(img.format), img.mipCount);
  return 0;
}

// windowmask <gameDir> <.material path> <out.png>: the night-lights window mask of a building material
// (engine/night_lights.cpp) over its darkened diffuse map, one colour per window group
int cmdWindowMask(int argc, char** argv) {
  if (argc < 5) return 2;
  Vfs vfs;
  vfs.setInstallRoot(widen(argv[2]));
  vfs.rescan();
  auto read = [&](const std::string& p, std::vector<uint8_t>& out) { return vfs.read(p, out); };
  render::MaterialDef m;
  if (!render::loadMaterialFile(argv[3], read, m)) {
    std::fprintf(stderr, "%s: cannot load\n", argv[3]);
    return 1;
  }
  for (size_t i = 0; i < m.textures.size(); ++i) std::printf("texture %zu: %s\n", i, m.textures[i].c_str());
  if (!(m.flags & render::kMatSpecularMap) || !(m.flags & render::kMatDiffuseMap)) {
    std::printf("no specular or diffuse map (flags 0x%x)\n", m.flags);
    return 1;
  }
  size_t at = 1 + ((m.flags & 0x400) ? 1 : 0) + ((m.flags & 0x800) ? 2 : 0);
  size_t il = at + 1 + ((m.flags & render::kMatNormalMap) ? 1 : 0);
  auto load = [&](const std::string& p, render::Image& img) {
    std::vector<uint8_t> data;
    return read(p, data) && render::parseDds(data, img, nullptr);
  };
  render::Image diffuse, spec, illum, mask;
  bool haveIllum = (m.flags & render::kMatIlluminationMap) && il < m.textures.size() && load(m.textures[il], illum);
  if (!load(m.textures[0], diffuse) || at >= m.textures.size() || !load(m.textures[at], spec)) return 1;
  engine::WindowMaskInfo info;
  bool ok = engine::makeWindowMask(spec, haveIllum ? &illum : nullptr, mask, &info);
  std::printf("specular %s: %d windows, %d rejected, glass > %.2f, %.1f %% of the texture%s%s\n", m.textures[at].c_str(),
              info.windows, info.rejected, info.threshold, info.coverage * 100, info.skipped ? ": " : "",
              info.skipped ? info.skipped : "");
  if (!ok) return 1;
  std::vector<uint8_t> drgba;
  int dw = 0, dh = 0, level = 0;
  while (level + 1 < diffuse.mipCount && (diffuse.width >> level) > mask.width) ++level;
  render::decodeRgba(diffuse, level, drgba, dw, dh);
  const int w = mask.width, h = mask.height;
  std::vector<uint8_t> out(size_t(w) * size_t(h) * 4);
  static const float col[4][3] = {{0.2f, 0.6f, 1.0f}, {0.3f, 1.0f, 0.3f}, {1.0f, 0.35f, 0.2f}, {1.0f, 0.9f, 0.2f}};
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const uint8_t* dp = &drgba[(size_t(y * dh / h) * size_t(dw) + size_t(x * dw / w)) * 4];
      const uint8_t* mp = &mask.levels[0][(size_t(y) * size_t(w) + size_t(x)) * 4];
      for (int c = 0; c < 3; ++c) {
        float v = dp[c] / 255.0f * 0.35f;
        for (int g = 0; g < 4; ++g) v += mp[g] / 255.0f * col[g][c] * 0.9f;
        out[(size_t(y) * size_t(w) + size_t(x)) * 4 + size_t(c)] = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f);
      }
      out[(size_t(y) * size_t(w) + size_t(x)) * 4 + 3] = 255;
    }
  std::vector<uint8_t> png = render::encodePng(out.data(), w, h);
  FILE* f = _wfopen(widen(argv[4]).wstring().c_str(), L"wb");
  if (!f) return 1;
  std::fwrite(png.data(), 1, png.size(), f);
  std::fclose(f);
  return 0;
}

// vehiclelamps <gameDir> <outDir> <.object path>...: the lamps found on vehicle textures (engine/night_lights.cpp),
// front lamps white and rear lamps red over the darkened diffuse map, one PNG per material
int cmdVehicleLamps(int argc, char** argv) {
  if (argc < 5) return 2;
  Vfs vfs;
  vfs.setInstallRoot(widen(argv[2]));
  vfs.rescan();
  auto read = [&](const std::string& p, std::vector<uint8_t>& out) { return vfs.read(p, out); };
  for (int a = 4; a < argc; ++a) {
    render::ObjectData od;
    std::string err;
    if (!render::loadObject(argv[a], read, od, &err)) {
      std::printf("%s: %s\n", argv[a], err.c_str());
      continue;
    }
    for (size_t mi = 0; mi < od.materials.size(); ++mi) {
      const render::MaterialDef& m = od.materials[mi];
      std::vector<uint8_t> data;
      render::Image di, si, mask;
      if (!(m.flags & render::kMatDiffuseMap) || m.textures.empty() || !read(m.textures[0], data) || !render::parseDds(data, di, nullptr))
        continue;
      size_t at = 1 + ((m.flags & 0x400) ? 1 : 0) + ((m.flags & 0x800) ? 2 : 0);
      bool spec = (m.flags & render::kMatSpecularMap) && at < m.textures.size() && read(m.textures[at], data) &&
                  render::parseDds(data, si, nullptr);
      engine::VehicleLampInfo info;
      bool ok;
      if (engine::isLampTemplate(argv[a])) {  // a street lamp: its glass
        int texels = 0;
        ok = engine::makeLampGlassMask(od.mesh, int(mi), engine::findLampHeads(od.mesh), di, spec ? &si : nullptr, mask, &texels);
        info.frontLamps = texels;
      } else {
        ok = engine::makeVehicleLampMask(od.mesh, int(mi), di, spec ? &si : nullptr, mask, &info);
      }
      std::printf("%s / %s: %d front, %d rear lamp areas\n", argv[a], m.name.c_str(), info.frontLamps, info.rearLamps);
      if (!ok) continue;
      std::vector<uint8_t> drgba;
      int dw = 0, dh = 0, level = 0;
      while (level + 1 < di.mipCount && (di.width >> level) > mask.width) ++level;
      render::decodeRgba(di, level, drgba, dw, dh);
      const int w = mask.width, h = mask.height;
      std::vector<uint8_t> out(size_t(w) * size_t(h) * 4);
      for (size_t i = 0; i < size_t(w) * size_t(h); ++i) {
        const uint8_t* mp = &mask.levels[0][i * 4];
        for (int c = 0; c < 3; ++c) {
          float v = drgba[i * 4 + size_t(c)] / 255.0f * 0.45f + mp[2] / 255.0f * 0.9f + (c == 0 ? mp[1] / 255.0f * 0.9f : 0.0f);
          out[i * 4 + size_t(c)] = uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f);
        }
        out[i * 4 + 3] = 255;
      }
      std::string name = argv[a];
      name = name.substr(name.find_last_of('/') + 1) + "-" + std::to_string(mi) + ".png";
      std::vector<uint8_t> png = render::encodePng(out.data(), w, h);
      if (FILE* f = std::fopen((std::string(argv[3]) + "/" + name).c_str(), "wb")) {
        std::fwrite(png.data(), 1, png.size(), f);
        std::fclose(f);
      }
    }
  }
  return 0;
}

// lamps <gameDir> <.object path>...: the lamp heads the night lights find on lamp models
int cmdLamps(int argc, char** argv) {
  if (argc < 4) return 2;
  Vfs vfs;
  vfs.setInstallRoot(widen(argv[2]));
  vfs.rescan();
  auto read = [&](const std::string& p, std::vector<uint8_t>& out) { return vfs.read(p, out); };
  for (int a = 3; a < argc; ++a) {
    render::ObjectData od;
    std::string err;
    if (!render::loadObject(argv[a], read, od, &err)) {
      std::printf("%s: %s\n", argv[a], err.c_str());
      continue;
    }
    std::printf("%s: lamp name %s, bounds y %.2f..%.2f, %zu vertices\n", argv[a], engine::isLampTemplate(argv[a]) ? "yes" : "no",
                od.mesh.boundsMin[1], od.mesh.boundsMax[1], od.mesh.vertexCount());
    std::vector<render::Vec3> heads = engine::findLampHeads(od.mesh);
    for (const render::Vec3& p : heads) std::printf("  head (%.2f, %.2f, %.2f)\n", p.x, p.y, p.z);
    // REGS_LAMP_VIEW=<dir>: front (x, y) and top (x, z) views of the triangles, downward faces orange, heads red
    if (const char* dir = std::getenv("REGS_LAMP_VIEW")) {
      const render::MeshData& m = od.mesh;
      const int S = 256;
      float span = 0;
      for (size_t i = 0; i < m.vertexCount(); ++i)
        span = std::max({span, std::fabs(m.positions[i * 3]), std::fabs(m.positions[i * 3 + 1]), std::fabs(m.positions[i * 3 + 2])});
      span = std::max(span, 1.0f) * 1.05f;
      std::vector<uint8_t> img(size_t(S) * 2 * S * 4, 255);
      auto plot = [&](int view, float a, float b, uint8_t r, uint8_t g, uint8_t bl) {
        int x = int((a / span * 0.5f + 0.5f) * (S - 1)) + view * S, y = view == 0 ? int((1 - b / span) * (S - 1)) : int((b / span * 0.5f + 0.5f) * (S - 1));
        if (x < 0 || y < 0 || x >= 2 * S || y >= S) return;
        uint8_t* p = &img[(size_t(y) * 2 * S + size_t(x)) * 4];
        p[0] = r, p[1] = g, p[2] = bl;
      };
      for (size_t k = 0; k + 2 < m.indices.size(); k += 3) {
        uint32_t t[3] = {m.indices[k], m.indices[k + 1], m.indices[k + 2]};
        float ny = (m.normals[t[0] * 3 + 1] + m.normals[t[1] * 3 + 1] + m.normals[t[2] * 3 + 1]) / 3;
        bool down = ny < -0.6f;
        for (int e = 0; e < 3; ++e) {
          const float* p0 = &m.positions[t[e] * 3];
          const float* p1 = &m.positions[t[(e + 1) % 3] * 3];
          for (int s = 0; s <= 40; ++s) {
            float f = s / 40.0f;
            float x = p0[0] + (p1[0] - p0[0]) * f, y = p0[1] + (p1[1] - p0[1]) * f, z = p0[2] + (p1[2] - p0[2]) * f;
            plot(0, x, y, down ? 255 : 90, down ? 140 : 90, down ? 0 : 90);
            plot(1, x, z, down ? 255 : 90, down ? 140 : 90, down ? 0 : 90);
          }
        }
      }
      for (const render::Vec3& h : heads)
        for (int dy = -3; dy <= 3; ++dy)
          for (int dx = -3; dx <= 3; ++dx) {
            plot(0, h.x + dx * span / S * 2, h.y + dy * span / S * 2, 230, 0, 0);
            plot(1, h.x + dx * span / S * 2, h.z + dy * span / S * 2, 230, 0, 0);
          }
      std::string name = argv[a];
      name = name.substr(name.find_last_of('/') + 1);
      std::vector<uint8_t> png = render::encodePng(img.data(), 2 * S, S);
      std::string out = std::string(dir) + "/" + name + ".png";
      if (FILE* f = std::fopen(out.c_str(), "wb")) {
        std::fwrite(png.data(), 1, png.size(), f);
        std::fclose(f);
      }
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::setvbuf(stdout, nullptr, _IONBF, 0);  // keep output when a crash ends the process
  if (argc >= 2) {
    std::string cmd = argv[1];
    int rc = 2;
    if (cmd == "vfs") rc = cmdVfs(argc, argv);
    else if (cmd == "parse-all") rc = cmdParseAll(argc, argv);
    else if (cmd == "parse") rc = cmdParse(argc, argv);
    else if (cmd == "run") rc = cmdRun(argc, argv);
    else if (cmd == "script") rc = cmdScript(argc, argv);
    else if (cmd == "shader") rc = cmdShader(argc, argv);
    else if (cmd == "shaders") rc = cmdShaders(argc, argv);
    else if (cmd == "shadowtest") rc = cmdShadowTest(argc, argv);
    else if (cmd == "skintest") rc = cmdSkinTest(argc, argv);
    else if (cmd == "sound") rc = cmdSound(argc, argv);
    else if (cmd == "cut") rc = cmdCut(argc, argv);
    else if (cmd == "softdot") rc = cmdSoftDot(argc, argv);
    else if (cmd == "leaf") rc = cmdLeaf(argc, argv);
    else if (cmd == "texpng") rc = cmdTexPng(argc, argv);
    else if (cmd == "materials") rc = cmdMaterials(argc, argv);
    else if (cmd == "windowmask") rc = cmdWindowMask(argc, argv);
    else if (cmd == "lamps") rc = cmdLamps(argc, argv);
    else if (cmd == "vehiclelamps") rc = cmdVehicleLamps(argc, argv);
    if (rc != 2) return rc;
  }
  std::fprintf(stderr,
               "usage:\n  regs-tool vfs <gameDir> stats|verify|list [prefix]|cat <path> [out]\n"
               "  regs-tool parse-all <gameDir> [maxShown]\n  regs-tool parse <file>\n");
  return 2;
}

