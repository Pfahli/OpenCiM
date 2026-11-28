// OpenCiM's Steam client: login, logon and the depot download (core/steam_client.h, docs/android.md).
// Field numbers from SteamDatabase/Protobufs (steammessages_base, _auth.steamclient, _clientserver_login,
// _clientserver_appinfo, _clientserver_2, _contentsystem.steamclient, content_manifest).
#include "core/steam_client.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <sstream>

#include "core/inflate.h"
#include "core/log.h"
#include "core/net_curl.h"
#include "core/proto.h"
#include "core/steam_crypto.h"

extern "C" {
#include "LzmaDec.h"
}
#include "zstd.h"

namespace regs::steam {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

// ---- EMsg and other numbers ----
constexpr uint32_t kProtoMask = 0x80000000u;
constexpr uint32_t kMulti = 1, kServiceMethodResponse = 147, kServiceMethodCallFromClient = 151, kHeartBeat = 703,
                   kLogOff = 706, kLogOnResponse = 751, kLoggedOff = 757, kDepotKey = 5438, kDepotKeyResponse = 5439,
                   kLogon = 5514, kPicsProductInfo = 8903, kPicsProductInfoResponse = 8904, kPicsAccessToken = 8905,
                   kPicsAccessTokenResponse = 8906, kServiceMethodNonAuthed = 9804, kHello = 9805;
constexpr uint32_t kProtocolVersion = 65580;
constexpr uint64_t kNoJob = ~0ull;
constexpr int kOsWindows10 = 16;

std::string eresultText(int r) {
  switch (r) {
    case 1: return "OK";
    case 2: return "the request failed";
    case 3: return "no connection to Steam";
    case 5: return "the account name or password is wrong";
    case 6: return "the account signed in elsewhere";
    case 9: return "the login request expired";
    case 15: return "access was denied";
    case 20: return "the Steam service is not available right now";
    case 27: return "the login expired, please sign in again";
    case 63: return "Steam denied the login (Steam Guard)";
    case 65: return "the Steam Guard code is wrong";
    case 84: return "too many attempts, please wait a few minutes";
    case 85: return "a Steam Guard code from the Steam app is needed";
    case 88: return "the Steam Guard code is wrong";
    default: return "Steam answered with error " + std::to_string(r);
  }
}

uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
void wr32(std::vector<uint8_t>& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}

// Steam's Adler-32 of chunks starts at 0, not 1
uint32_t steamAdler(std::span<const uint8_t> d) {
  uint32_t a = 0, b = 0;
  for (size_t i = 0; i < d.size();) {
    size_t n = std::min<size_t>(d.size() - i, 5552);
    for (size_t k = 0; k < n; ++k) a += d[i + k], b += a;
    a %= 65521, b %= 65521;
    i += n;
  }
  return a | (b << 16);
}

// ---- gzip, zip and Steam's VZip (LZMA) ----
bool gunzip(std::span<const uint8_t> in, std::vector<uint8_t>& out) {
  if (in.size() < 18 || in[0] != 0x1f || in[1] != 0x8b || in[2] != 8) return false;
  const uint8_t flags = in[3];
  size_t p = 10;
  if (flags & 4) {
    if (p + 2 > in.size()) return false;
    p += 2 + (in[p] | in[p + 1] << 8);
  }
  if (flags & 8) while (p < in.size() && in[p++]) {}
  if (flags & 16) while (p < in.size() && in[p++]) {}
  if (flags & 2) p += 2;
  if (p + 8 > in.size()) return false;
  out.clear();
  return rawInflate(in.subspan(p, in.size() - p - 8), out, rd32(in.data() + in.size() - 4));
}

// the first file of a zip archive (manifests, zip-compressed chunks)
bool unzipFirst(std::span<const uint8_t> in, std::vector<uint8_t>& out) {
  if (in.size() < 22) return false;
  size_t eocd = std::string::npos;
  for (size_t i = in.size() - 22 + 1; i-- > 0 && in.size() - i <= 22 + 65535;)
    if (rd32(in.data() + i) == 0x06054b50) {
      eocd = i;
      break;
    }
  if (eocd == std::string::npos) return false;
  const size_t cd = rd32(in.data() + eocd + 16);
  if (cd + 46 > in.size() || rd32(in.data() + cd) != 0x02014b50) return false;
  const uint8_t* c = in.data() + cd;
  const int method = c[10] | c[11] << 8;
  const uint32_t csize = rd32(c + 20), usize = rd32(c + 24), lho = rd32(c + 42);
  if (lho + 30 > in.size() || rd32(in.data() + lho) != 0x04034b50) return false;
  const uint8_t* l = in.data() + lho;
  const size_t data = lho + 30 + (l[26] | l[27] << 8) + (l[28] | l[29] << 8);
  if (data + csize > in.size()) return false;
  out.clear();
  if (method == 0) {
    out.assign(in.begin() + data, in.begin() + data + csize);
    return true;
  }
  if (method != 8) return false;
  return rawInflate(in.subspan(data, csize), out, usize) && out.size() == usize;
}

void* lzAlloc(ISzAllocPtr, size_t n) { return std::malloc(n); }
void lzFree(ISzAllocPtr, void* p) { std::free(p); }

bool unVZip(std::span<const uint8_t> in, std::vector<uint8_t>& out) {
  // "VZa", 4 bytes (timestamp or CRC), 5 bytes LZMA properties, the stream, CRC32, size, "zv"
  if (in.size() < 22 || in[0] != 'V' || in[1] != 'Z' || in[2] != 'a') return false;
  if (in[in.size() - 2] != 'z' || in[in.size() - 1] != 'v') return false;
  const uint32_t size = rd32(in.data() + in.size() - 6);
  out.resize(size);
  SizeT outLen = size, inLen = in.size() - 12 - 10;
  ELzmaStatus st;
  ISzAlloc alloc{lzAlloc, lzFree};
  SRes r = LzmaDecode(out.data(), &outLen, in.data() + 12, &inLen, in.data() + 7, LZMA_PROPS_SIZE, LZMA_FINISH_ANY, &st, &alloc);
  return r == SZ_OK && outLen == size;
}

// Steam's VZstd: "VSZa", 4 bytes (CRC), one zstd frame, a footer (CRC, size, "zsv"); the frame's own length marks
// where the footer starts. size: the chunk's size from the manifest.
bool unVZstd(std::span<const uint8_t> in, std::vector<uint8_t>& out, size_t size) {
  if (in.size() < 8 + 9 || std::memcmp(in.data(), "VSZa", 4) != 0) return false;
  const size_t frame = ZSTD_findFrameCompressedSize(in.data() + 8, in.size() - 8);
  if (ZSTD_isError(frame)) return false;
  out.resize(size);
  const size_t n = ZSTD_decompress(out.data(), out.size(), in.data() + 8, frame);
  return !ZSTD_isError(n) && n == size;
}

// ---- text KeyValues (the app info) ----
struct KV {
  std::string key, value;
  std::vector<KV> kids;
  bool object = false;
  const KV* child(std::string_view k) const {
    for (const KV& c : kids)
      if (c.key.size() == k.size() && std::equal(c.key.begin(), c.key.end(), k.begin(), [](char a, char b) {
            return std::tolower(uint8_t(a)) == std::tolower(uint8_t(b));
          }))
        return &c;
    return nullptr;
  }
  std::string get(std::string_view k) const {
    const KV* c = child(k);
    return c && !c->object ? c->value : std::string();
  }
};

class KVParser {
 public:
  explicit KVParser(std::string_view s) : s_(s) {}
  bool parse(KV& root) {
    root.object = true;
    return body(root, true);
  }

 private:
  bool token(std::string& t, bool& quoted) {
    while (p_ < s_.size()) {
      char c = s_[p_];
      if (std::isspace(uint8_t(c)) || c == '\0') {
        ++p_;
      } else if (c == '/' && p_ + 1 < s_.size() && s_[p_ + 1] == '/') {
        while (p_ < s_.size() && s_[p_] != '\n') ++p_;
      } else {
        break;
      }
    }
    if (p_ >= s_.size()) return false;
    t.clear();
    quoted = false;
    char c = s_[p_];
    if (c == '{' || c == '}') {
      t = c, ++p_;
      return true;
    }
    if (c == '"') {
      quoted = true;
      ++p_;
      while (p_ < s_.size() && s_[p_] != '"') {
        if (s_[p_] == '\\' && p_ + 1 < s_.size()) {
          char e = s_[++p_];
          t += e == 'n' ? '\n' : e == 't' ? '\t' : e;
          ++p_;
        } else {
          t += s_[p_++];
        }
      }
      ++p_;
      return true;
    }
    while (p_ < s_.size() && !std::isspace(uint8_t(s_[p_])) && s_[p_] != '{' && s_[p_] != '}' && s_[p_] != '"') t += s_[p_++];
    return true;
  }
  bool body(KV& into, bool top) {
    std::string t;
    bool q;
    while (token(t, q)) {
      if (!q && t == "}") return !top;
      KV kv;
      kv.key = t;
      std::string v;
      if (!token(v, q)) return false;
      if (!q && v == "{") {
        kv.object = true;
        if (!body(kv, false)) return false;
      } else {
        kv.value = v;
      }
      into.kids.push_back(std::move(kv));
    }
    return top;
  }
  std::string_view s_;
  size_t p_ = 0;
};

// ---- a CM message ----
struct Header {
  uint64_t steamid = 0;
  int32_t sessionid = 0;
  uint64_t jobSource = kNoJob, jobTarget = kNoJob;
  std::string jobName;
  int eresult = 2;
  std::string errorMessage;
};

struct Msg {
  uint32_t emsg = 0;
  Header hdr;
  std::vector<uint8_t> body;
};

Header parseHeader(std::span<const uint8_t> d) {
  Header h;
  for (pb::Reader r(d); r.next();) {
    const pb::Field& f = r.field;
    switch (f.number) {
      case 1: h.steamid = f.value; break;
      case 2: h.sessionid = int32_t(f.value); break;
      case 10: h.jobSource = f.value; break;
      case 11: h.jobTarget = f.value; break;
      case 12: h.jobName = f.str(); break;
      case 13: h.eresult = int32_t(f.value); break;
      case 14: h.errorMessage = f.str(); break;
    }
  }
  return h;
}

struct Cancelled {};

// The license list and what else a CM sends unasked is not needed: messages are taken by job id or type.
class Cm {
 public:
  explicit Cm(const std::atomic<bool>& cancel) : cancel_(cancel) {}

  bool connect(std::string& error) {
    net::Http http;
    std::vector<uint8_t> body;
    std::vector<std::string> hosts;
    if (http.get("https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/?cellid=0&cmtype=websockets",
                 body, &error, nullptr, &cancel_, 20)) {
      // {"response":{"serverlist":[{"endpoint":"host:port","type":"websockets",...},...],...}}
      std::string s(body.begin(), body.end());
      for (size_t p = 0; (p = s.find("\"endpoint\"", p)) != std::string::npos;) {
        size_t a = s.find('"', s.find(':', p) + 1), b = a == std::string::npos ? a : s.find('"', a + 1);
        if (b == std::string::npos) break;
        std::string ep = s.substr(a + 1, b - a - 1);
        size_t t = s.find("\"type\"", b);
        size_t next = s.find("\"endpoint\"", b);
        bool ws = t != std::string::npos && (next == std::string::npos || t < next) &&
                  s.compare(s.find('"', s.find(':', t) + 1) + 1, 10, "websockets") == 0;
        if (ws) hosts.push_back(ep);
        p = b;
      }
    }
    if (hosts.empty()) hosts = {"cmp1-fra1.steamserver.net:443", "cmp2-fra1.steamserver.net:443",
                                "cmp1-ams1.steamserver.net:443"};
    for (size_t i = 0; i < hosts.size() && i < 5; ++i) {
      if (cancel_) throw Cancelled{};
      if (ws_.connect("wss://" + hosts[i] + "/cmsocket/", &error)) {
        LOG_INFO("steam: connected to %s", hosts[i].c_str());
        pb::Writer hello;
        hello.u64(1, kProtocolVersion);
        send(kHello, hello);
        return true;
      }
      LOG_WARN("steam: %s: %s", hosts[i].c_str(), error.c_str());
    }
    if (error.empty()) error = "no Steam server answered";
    return false;
  }

  void setSession(uint64_t steamid, int32_t session) { steamid_ = steamid, session_ = session; }
  uint64_t steamid() const { return steamid_; }
  void setHeartbeat(int seconds) { heartbeat_ = seconds, lastBeat_ = Clock::now(); }

  uint64_t newJob() { return ++job_; }

  void send(uint32_t emsg, const pb::Writer& body, uint64_t jobSource = kNoJob, const std::string& jobName = {}) {
    pb::Writer h;
    if (steamid_) h.fixed64(1, steamid_);
    if (session_) h.i32(2, session_);
    if (jobSource != kNoJob) h.fixed64(10, jobSource);
    if (!jobName.empty()) h.str(12, jobName);
    std::vector<uint8_t> m;
    wr32(m, emsg | kProtoMask);
    wr32(m, uint32_t(h.buf.size()));
    m.insert(m.end(), h.buf.begin(), h.buf.end());
    m.insert(m.end(), body.buf.begin(), body.buf.end());
    std::string err;
    if (!ws_.send(m, &err)) throw std::runtime_error("lost the connection to Steam (" + err + ")");
  }

  // the next message that `want` takes, up to timeoutMs; nullopt on timeout
  std::optional<Msg> waitFor(const std::function<bool(const Msg&)>& want, int timeoutMs) {
    const auto until = Clock::now() + std::chrono::milliseconds(timeoutMs);
    while (true) {
      for (auto it = queue_.begin(); it != queue_.end(); ++it)
        if (want(*it)) {
          Msg m = std::move(*it);
          queue_.erase(it);
          return m;
        }
      if (cancel_) throw Cancelled{};
      beat();
      const int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(until - Clock::now()).count());
      if (left <= 0) return std::nullopt;
      std::vector<uint8_t> raw;
      std::string err;
      int r = ws_.receive(raw, std::min(left, 500), &err);
      if (r < 0) throw std::runtime_error("lost the connection to Steam (" + err + ")");
      if (r > 0) take(raw);
    }
  }

  Msg waitJob(uint64_t job, uint32_t emsg, int timeoutMs = 30000) {
    auto m = waitFor([&](const Msg& x) { return x.hdr.jobTarget == job && (emsg == 0 || x.emsg == emsg); }, timeoutMs);
    if (!m) throw std::runtime_error("Steam did not answer in time");
    return std::move(*m);
  }

  // a service method ("Authentication.GetPasswordRSAPublicKey#1"); the answer's body, its eresult in eresult
  std::vector<uint8_t> call(const std::string& method, const pb::Writer& body, int& eresult, bool authed = true) {
    const uint64_t job = newJob();
    send(authed ? kServiceMethodCallFromClient : kServiceMethodNonAuthed, body, job, method);
    Msg m = waitJob(job, kServiceMethodResponse);
    eresult = m.hdr.eresult;
    if (eresult != 1) LOG_WARN("steam: %s: eresult %d %s", method.c_str(), eresult, m.hdr.errorMessage.c_str());
    return std::move(m.body);
  }

  void close() { ws_.close(); }

 private:
  void beat() {
    if (heartbeat_ > 0 && Clock::now() - lastBeat_ > std::chrono::seconds(heartbeat_)) {
      lastBeat_ = Clock::now();
      send(kHeartBeat, pb::Writer{});
    }
  }
  void take(std::span<const uint8_t> raw) {
    if (raw.size() < 8) return;
    const uint32_t e = rd32(raw.data());
    if (!(e & kProtoMask)) return;  // (only protobuf messages arrive on the WebSocket connection)
    const uint32_t hl = rd32(raw.data() + 4);
    if (8 + size_t(hl) > raw.size()) return;
    Msg m;
    m.emsg = e & ~kProtoMask;
    m.hdr = parseHeader(raw.subspan(8, hl));
    m.body.assign(raw.begin() + 8 + hl, raw.end());
    if (m.emsg == kMulti) {
      uint32_t unzipped = 0;
      std::vector<uint8_t> payload;
      for (pb::Reader r(m.body); r.next();) {
        if (r.field.number == 1) unzipped = uint32_t(r.field.value);
        if (r.field.number == 2) payload = r.field.blob();
      }
      if (unzipped) {
        std::vector<uint8_t> z;
        if (!gunzip(payload, z)) return;
        payload.swap(z);
      }
      for (size_t p = 0; p + 4 <= payload.size();) {
        const uint32_t n = rd32(payload.data() + p);
        if (p + 4 + n > payload.size()) break;
        take(std::span<const uint8_t>(payload).subspan(p + 4, n));
        p += 4 + n;
      }
      return;
    }
    if (m.emsg == kLoggedOff) {
      int r = 2;
      for (pb::Reader rd(m.body); rd.next();)
        if (rd.field.number == 1) r = int(rd.field.value);
      throw std::runtime_error("Steam ended the session (" + eresultText(r) + ")");
    }
    if (queue_.size() > 200) queue_.pop_front();
    queue_.push_back(std::move(m));
  }

  const std::atomic<bool>& cancel_;
  net::WebSocket ws_;
  std::deque<Msg> queue_;
  uint64_t steamid_ = 0;
  int32_t session_ = 0;
  uint64_t job_ = (uint64_t(std::time(nullptr)) & 0xFFFFFu) << 32;
  int heartbeat_ = 0;
  Clock::time_point lastBeat_ = Clock::now();
};

struct ChunkInfo {
  std::array<uint8_t, 20> sha{};
  uint32_t crc = 0;
  uint64_t offset = 0;
  uint32_t size = 0, compressed = 0;
};

struct FileEntry {
  std::string name;  // '/' separated
  uint64_t size = 0;
  uint32_t flags = 0;
  std::vector<ChunkInfo> chunks;
};

struct Depot {
  uint32_t id = 0;
  uint64_t manifest = 0;
  std::vector<uint8_t> key;
  std::vector<FileEntry> files;
};

}  // namespace

// ---- the downloader ----
struct Downloader::Impl {
  Options opt;
  mutable std::mutex mu;
  std::condition_variable codeCv;
  Status st;
  std::string pendingCode;
  std::atomic<bool> cancel{false};
  std::thread thread;
  std::atomic<bool> busy{false};

  void set(const std::function<void(Status&)>& f) {
    std::lock_guard<std::mutex> lk(mu);
    f(st);
    ++st.sequence;
  }
  void phase(Phase p, const std::string& message) {
    LOG_INFO("steam: %s", message.c_str());
    set([&](Status& s) { s.phase = p, s.message = message; });
  }
  std::string takeCode() {
    std::lock_guard<std::mutex> lk(mu);
    std::string c;
    c.swap(pendingCode);
    return c;
  }

  // ---- the saved login ----
  struct Saved {
    std::string account, token;
  };
  static std::optional<Saved> load(const fs::path& f) {
    std::ifstream in(f);
    if (!in) return std::nullopt;
    Saved s;
    std::string line;
    while (std::getline(in, line)) {
      if (line.rfind("account=", 0) == 0) s.account = line.substr(8);
      if (line.rfind("token=", 0) == 0) s.token = line.substr(6);
    }
    if (s.token.empty()) return std::nullopt;
    return s;
  }
  void save(const std::string& account, const std::string& token) {
    if (opt.tokenFile.empty()) return;
    std::error_code ec;
    fs::create_directories(opt.tokenFile.parent_path(), ec);
    std::ofstream out(opt.tokenFile, std::ios::trunc);
    out << "account=" << account << "\ntoken=" << token << "\n";
  }

  // ---- login ----
  pb::Writer deviceDetails() {
    pb::Writer d;
    d.str(1, opt.deviceName);
    d.u64(2, 1);  // k_EAuthTokenPlatformType_SteamClient
    d.i32(3, kOsWindows10);
    d.u64(4, 1);
    return d;
  }

  // polls an auth session until it gives the refresh token; QR sessions show (new) challenge URLs, credential sessions
  // take Steam Guard codes from submitCode()
  std::string poll(Cm& cm, uint64_t& clientId, const std::vector<uint8_t>& requestId, float interval, uint64_t steamid,
                   int codeType, std::string& account) {
    auto next = Clock::now();
    const auto giveUp = Clock::now() + std::chrono::minutes(10);
    while (true) {
      if (cancel) throw Cancelled{};
      if (Clock::now() > giveUp) throw std::runtime_error("the login was not confirmed in time");
      if (codeType) {
        std::string code = takeCode();
        if (!code.empty()) {
          phase(Phase::SigningIn, "checking the code");
          pb::Writer u;
          u.u64(1, clientId);
          u.fixed64(2, steamid);
          u.str(3, code);
          u.u64(4, uint64_t(codeType));
          int er = 2;
          cm.call("Authentication.UpdateAuthSessionWithSteamGuardCode#1", u, er, false);
          if (er == 65 || er == 88) {
            set([&](Status& s) {
              s.phase = Phase::NeedCode, s.codeWrong = true, s.message = "the code was not accepted, please try again";
            });
            continue;
          }
          if (er != 1 && er != 29) throw std::runtime_error(eresultText(er));
          next = Clock::now();
        }
      }
      if (Clock::now() < next) {
        cm.waitFor([](const Msg&) { return false; }, 200);  // keeps the connection read and alive
        continue;
      }
      next = Clock::now() + std::chrono::milliseconds(int(std::max(1.0f, interval) * 1000));
      pb::Writer p;
      p.u64(1, clientId);
      p.bytes(2, requestId);
      int er = 2;
      auto body = cm.call("Authentication.PollAuthSessionStatus#1", p, er, false);
      if (er != 1) throw std::runtime_error(er == 9 ? std::string("the login request expired, please try again") : eresultText(er));
      std::string refresh;
      for (pb::Reader r(body); r.next();) {
        switch (r.field.number) {
          case 1: clientId = r.field.value; break;
          case 2: {
            std::string url = r.field.str();
            set([&](Status& s) { s.qrUrl = url; });
            break;
          }
          case 3: refresh = r.field.str(); break;
          case 5:
            if (r.field.value && !codeType)
              set([&](Status& s) { s.message = "confirm the login in the Steam app"; });
            break;
          case 6: account = r.field.str(); break;
        }
      }
      if (!refresh.empty()) return refresh;
    }
  }

  std::string loginPassword(Cm& cm, const std::string& account, const std::string& password, std::string& accountOut) {
    phase(Phase::SigningIn, "signing in to Steam");
    pb::Writer k;
    k.str(1, account);
    int er = 2;
    auto kb = cm.call("Authentication.GetPasswordRSAPublicKey#1", k, er, false);
    if (er != 1) throw std::runtime_error(eresultText(er));
    std::string mod, exp;
    uint64_t ts = 0;
    for (pb::Reader r(kb); r.next();) {
      if (r.field.number == 1) mod = r.field.str();
      if (r.field.number == 2) exp = r.field.str();
      if (r.field.number == 3) ts = r.field.value;
    }
    const std::string enc = steamcrypto::rsaEncryptBase64(mod, exp, password);
    if (enc.empty()) throw std::runtime_error("the password could not be encrypted");
    pb::Writer b;
    b.str(1, opt.deviceName);
    b.str(2, account);
    b.str(3, enc);
    b.u64(4, ts);
    b.boolean(5, true);
    b.u64(6, 1);  // SteamClient
    b.u64(7, 1);  // persistent
    b.str(8, "Client");
    b.msg(9, deviceDetails());
    auto rb = cm.call("Authentication.BeginAuthSessionViaCredentials#1", b, er, false);
    if (er != 1) throw std::runtime_error(eresultText(er));
    uint64_t clientId = 0, steamid = 0;
    std::vector<uint8_t> requestId;
    float interval = 5;
    std::vector<std::pair<int, std::string>> confirmations;
    for (pb::Reader r(rb); r.next();) {
      switch (r.field.number) {
        case 1: clientId = r.field.value; break;
        case 2: requestId = r.field.blob(); break;
        case 3: interval = r.field.f32(); break;
        case 4: {
          int type = 0;
          std::string msg;
          for (pb::Reader c(r.field.data); c.next();) {
            if (c.field.number == 1) type = int(c.field.value);
            if (c.field.number == 2) msg = c.field.str();
          }
          confirmations.emplace_back(type, msg);
          break;
        }
        case 5: steamid = r.field.value; break;
      }
    }
    auto has = [&](int t) {
      for (auto& [ct, m] : confirmations)
        if (ct == t) return true;
      return false;
    };
    auto hint = [&](int t) {
      for (auto& [ct, m] : confirmations)
        if (ct == t) return m;
      return std::string();
    };
    int codeType = 0;
    if (has(1)) {
      phase(Phase::SigningIn, "signing in to Steam");
    } else if (has(4)) {
      codeType = has(3) ? 3 : 0;
      set([&](Status& s) {
        s.phase = Phase::ConfirmInApp, s.allowCode = codeType != 0, s.code = codeType ? CodeKind::Authenticator : CodeKind::None;
        s.message = "confirm the login in the Steam app on your phone";
      });
    } else if (has(3)) {
      codeType = 3;
      set([&](Status& s) {
        s.phase = Phase::NeedCode, s.code = CodeKind::Authenticator, s.message = "enter the code from the Steam app";
      });
    } else if (has(2)) {
      codeType = 2;
      const std::string domain = hint(2);
      set([&](Status& s) {
        s.phase = Phase::NeedCode, s.code = CodeKind::Email, s.codeHint = domain;
        s.message = "enter the code Steam sent to your e-mail" + (domain.empty() ? std::string() : " (" + domain + ")");
      });
    } else if (has(5)) {
      phase(Phase::ConfirmInApp, "confirm the login in the e-mail Steam sent you");
    }
    return poll(cm, clientId, requestId, interval, steamid, codeType, accountOut);
  }

  std::string loginQr(Cm& cm, std::string& accountOut) {
    phase(Phase::SigningIn, "asking Steam for a QR code");
    pb::Writer b;
    b.str(1, opt.deviceName);
    b.u64(2, 1);
    b.msg(3, deviceDetails());
    b.str(4, "Client");
    int er = 2;
    auto rb = cm.call("Authentication.BeginAuthSessionViaQR#1", b, er, false);
    if (er != 1) throw std::runtime_error(eresultText(er));
    uint64_t clientId = 0;
    std::vector<uint8_t> requestId;
    float interval = 5;
    std::string url;
    for (pb::Reader r(rb); r.next();) {
      if (r.field.number == 1) clientId = r.field.value;
      if (r.field.number == 2) url = r.field.str();
      if (r.field.number == 3) requestId = r.field.blob();
      if (r.field.number == 4) interval = r.field.f32();
    }
    set([&](Status& s) {
      s.phase = Phase::ShowQr, s.qrUrl = url, s.message = "scan the code with the Steam app (Steam Guard > the QR symbol)";
    });
    return poll(cm, clientId, requestId, interval, 0, 0, accountOut);
  }

  // the client logon: with a refresh token, or anonymous (token empty)
  void logon(Cm& cm, const std::string& account, const std::string& token) {
    phase(Phase::SigningIn, account.empty() ? "logging on anonymously" : "logging on as " + account);
    // the session's SteamID before the logon: account 0, instance 1 (desktop), public universe, individual; anonymous
    // users have their own account type (10) and instance 0
    cm.setSession(token.empty() ? 0x01A0000000000000ull : 0x0110000100000000ull, 0);
    pb::Writer l;
    l.u64(1, kProtocolVersion);
    l.u64(3, 0);
    l.u64(5, 1771);
    l.str(6, "english");
    l.u64(7, uint64_t(uint32_t(kOsWindows10)));
    if (!token.empty()) {
      l.boolean(8, true);
      l.str(50, account);
      l.str(96, opt.deviceName);
      l.boolean(102, true);
      l.str(108, token);
    }
    cm.send(kLogon, l);
    auto m = cm.waitFor([](const Msg& x) { return x.emsg == kLogOnResponse; }, 30000);
    if (!m) throw std::runtime_error("Steam did not answer the logon");
    int er = 2, beat = 9;
    for (pb::Reader r(m->body); r.next();) {
      if (r.field.number == 1) er = int(r.field.value);
      if (r.field.number == 3) beat = int(r.field.value);
      if (r.field.number == 7) cellId = uint32_t(r.field.value);
    }
    if (er != 1) {
      if (!token.empty() && (er == 5 || er == 27 || er == 63 || er == 15)) {
        Downloader::forgetLogin(opt.tokenFile);
        throw std::runtime_error("the saved Steam login is no longer valid, please sign in again");
      }
      throw std::runtime_error("Steam refused the logon: " + eresultText(er));
    }
    cm.setSession(m->hdr.steamid, m->hdr.sessionid);
    cm.setHeartbeat(std::max(5, beat));
    LOG_INFO("steam: logged on (cell %u)", cellId);
  }

  uint32_t cellId = 0;

  // ---- app info and depots ----
  KV appInfo(Cm& cm) {
    uint64_t token = 0;
    {
      pb::Writer t;
      t.u64(2, opt.appId);
      const uint64_t job = cm.newJob();
      cm.send(kPicsAccessToken, t, job);
      Msg m = cm.waitJob(job, kPicsAccessTokenResponse);
      for (pb::Reader r(m.body); r.next();)
        if (r.field.number == 3)
          for (pb::Reader a(r.field.data); a.next();)
            if (a.field.number == 2) token = a.field.value;
    }
    pb::Writer app;
    app.u64(1, opt.appId);
    if (token) app.u64(2, token);
    pb::Writer q;
    q.msg(2, app);
    q.boolean(3, false);
    const uint64_t job = cm.newJob();
    cm.send(kPicsProductInfo, q, job);
    std::string text;
    while (true) {
      Msg m = cm.waitJob(job, kPicsProductInfoResponse);
      bool pending = false;
      std::string httpHost;
      std::vector<uint8_t> sha;
      for (pb::Reader r(m.body); r.next();) {
        if (r.field.number == 1)
          for (pb::Reader a(r.field.data); a.next();) {
            if (a.field.number == 4) sha = a.field.blob();
            if (a.field.number == 5) text = a.field.str();
          }
        if (r.field.number == 6) pending = r.field.value != 0;
        if (r.field.number == 8) httpHost = r.field.str();
      }
      if (text.empty() && !httpHost.empty() && !sha.empty()) {  // large app infos come over HTTP
        net::Http http;
        std::vector<uint8_t> gz, plain;
        std::string err;
        if (http.get("https://" + httpHost + "/appinfo/" + std::to_string(opt.appId) + "/sha/" + steamcrypto::hex(sha) +
                         ".txt.gz",
                     gz, &err, nullptr, &cancel) &&
            gunzip(gz, plain))
          text.assign(plain.begin(), plain.end());
      }
      if (!pending) break;
    }
    KV root;
    if (text.empty() || !KVParser(text).parse(root)) throw std::runtime_error("Steam sent no information about the game");
    const KV* appinfo = root.kids.empty() ? nullptr : &root.kids.front();
    if (!appinfo || !appinfo->object) throw std::runtime_error("Steam's information about the game is unreadable");
    return *appinfo;
  }

  std::vector<Depot> depots(Cm& cm, const KV& app) {
    const KV* ds = app.child("depots");
    if (!ds) throw std::runtime_error("the game has no depots on Steam");
    std::vector<Depot> out;
    for (const KV& d : ds->kids) {
      if (!d.object || d.key.empty() || !std::isdigit(uint8_t(d.key[0]))) continue;
      Depot dep;
      dep.id = uint32_t(std::strtoul(d.key.c_str(), nullptr, 10));
      if (!opt.onlyDepots.empty() && std::find(opt.onlyDepots.begin(), opt.onlyDepots.end(), dep.id) == opt.onlyDepots.end())
        continue;
      if (d.child("depotfromapp")) continue;  // shared redistributables
      if (const KV* cfg = d.child("config")) {
        const std::string os = cfg->get("oslist");
        if (!os.empty() && os.find(opt.os) == std::string::npos) continue;
        const std::string lang = cfg->get("language");
        if (!lang.empty() && lang != "english") continue;
      }
      const KV* manifests = d.child("manifests");
      const KV* pub = manifests ? manifests->child("public") : nullptr;
      if (!pub) continue;
      dep.manifest = std::strtoull((pub->object ? pub->get("gid") : pub->value).c_str(), nullptr, 10);
      if (!dep.manifest) continue;
      // the depot key: only for depots the account owns (the DLCs it does not own end here)
      pb::Writer k;
      k.u64(1, dep.id);
      k.u64(2, opt.appId);
      const uint64_t job = cm.newJob();
      cm.send(kDepotKey, k, job);
      Msg m = cm.waitJob(job, kDepotKeyResponse);
      int er = 2;
      for (pb::Reader r(m.body); r.next();) {
        if (r.field.number == 1) er = int(r.field.value);
        if (r.field.number == 3) dep.key = r.field.blob();
      }
      const std::string name = d.get("name");
      if (er != 1 || dep.key.size() != 32) {
        LOG_INFO("steam: depot %u (%s) skipped: %s", dep.id, name.c_str(), eresultText(er).c_str());
        continue;
      }
      LOG_INFO("steam: depot %u (%s), manifest %llu", dep.id, name.c_str(), (unsigned long long)dep.manifest);
      out.push_back(std::move(dep));
    }
    return out;
  }

  std::vector<std::string> cdnServers(Cm& cm) {
    pb::Writer q;
    q.u64(1, cellId);
    q.u64(2, 20);
    int er = 2;
    auto body = cm.call("ContentServerDirectory.GetServersForSteamPipe#1", q, er);
    std::vector<std::string> out;
    for (pb::Reader r(body); r.next();) {
      if (r.field.number != 1) continue;
      std::string type, host, vhost, https;
      bool restricted = false;
      for (pb::Reader s(r.field.data); s.next();) {
        switch (s.field.number) {
          case 1: type = s.field.str(); break;
          case 8: host = s.field.str(); break;
          case 9: vhost = s.field.str(); break;
          case 12: https = s.field.str(); break;
          case 13: restricted = true; break;
        }
      }
      if ((type == "CDN" || type == "SteamCache") && !restricted && !vhost.empty())
        out.push_back((https == "none" ? "http://" : "https://") + vhost);
    }
    if (out.empty()) out = {"https://cache1-fra1.steamcontent.com", "https://cache2-fra1.steamcontent.com"};
    return out;
  }

  void manifest(Cm& cm, Depot& dep, const std::vector<std::string>& cdns) {
    pb::Writer q;
    q.u64(1, opt.appId);
    q.u64(2, dep.id);
    q.u64(3, dep.manifest);
    q.str(4, "public");
    int er = 2;
    auto body = cm.call("ContentServerDirectory.GetManifestRequestCode#1", q, er);
    uint64_t code = 0;
    for (pb::Reader r(body); r.next();)
      if (r.field.number == 1) code = r.field.value;
    net::Http http;
    std::vector<uint8_t> zip, data;
    std::string err;
    bool got = false;
    for (size_t i = 0; i < cdns.size() && !got; ++i) {
      std::string url = cdns[i] + "/depot/" + std::to_string(dep.id) + "/manifest/" + std::to_string(dep.manifest) + "/5";
      if (code) url += "/" + std::to_string(code);
      got = http.get(url, zip, &err, nullptr, &cancel) && unzipFirst(zip, data);
    }
    if (!got) throw std::runtime_error("the file list of depot " + std::to_string(dep.id) + " could not be loaded (" + err + ")");
    // sections: magic, length, protobuf; the end magic has no length
    bool encrypted = false;
    std::span<const uint8_t> payload;
    for (size_t p = 0; p + 4 <= data.size();) {
      const uint32_t magic = rd32(data.data() + p);
      if (magic == 0x32C415ABu) break;
      if (p + 8 > data.size()) break;
      const uint32_t n = rd32(data.data() + p + 4);
      if (p + 8 + n > data.size()) break;
      auto sec = std::span<const uint8_t>(data).subspan(p + 8, n);
      if (magic == 0x71F617D0u) payload = sec;
      if (magic == 0x1F4812BEu)
        for (pb::Reader r(sec); r.next();)
          if (r.field.number == 4) encrypted = r.field.value != 0;
      p += 8 + n;
    }
    for (pb::Reader r(payload); r.next();) {
      if (r.field.number != 1) continue;
      FileEntry f;
      for (pb::Reader m(r.field.data); m.next();) {
        switch (m.field.number) {
          case 1: f.name = m.field.str(); break;
          case 2: f.size = m.field.value; break;
          case 3: f.flags = uint32_t(m.field.value); break;
          case 6: {
            ChunkInfo c;
            for (pb::Reader cr(m.field.data); cr.next();) {
              switch (cr.field.number) {
                case 1:
                  if (cr.field.data.size() == 20) std::copy(cr.field.data.begin(), cr.field.data.end(), c.sha.begin());
                  break;
                case 2: c.crc = uint32_t(cr.field.value); break;
                case 3: c.offset = cr.field.value; break;
                case 4: c.size = uint32_t(cr.field.value); break;
                case 5: c.compressed = uint32_t(cr.field.value); break;
              }
            }
            f.chunks.push_back(c);
            break;
          }
          case 7: f.flags |= 0x80000000u; break;  // a link: left out
        }
      }
      if (encrypted) {
        std::vector<uint8_t> plain;
        if (!steamcrypto::symmetricDecrypt(steamcrypto::base64Decode(f.name), dep.key, plain))
          throw std::runtime_error("the file names of depot " + std::to_string(dep.id) + " could not be decrypted");
        f.name.assign(plain.begin(), plain.end());
      }
      while (!f.name.empty() && f.name.back() == '\0') f.name.pop_back();
      std::replace(f.name.begin(), f.name.end(), '\\', '/');
      if (f.name.empty() || f.name.find("..") != std::string::npos || f.name[0] == '/') continue;
      std::sort(f.chunks.begin(), f.chunks.end(), [](const ChunkInfo& a, const ChunkInfo& b) { return a.offset < b.offset; });
      dep.files.push_back(std::move(f));
    }
    LOG_INFO("steam: depot %u: %zu entries", dep.id, dep.files.size());
  }

  // ---- the download ----
  struct Job {
    const Depot* depot;
    const FileEntry* file;
    const ChunkInfo* chunk;
  };

  void download(std::vector<Depot>& deps, const std::vector<std::string>& cdns) {
    std::error_code ec;
    fs::create_directories(opt.gameDir, ec);
    phase(Phase::Preparing, "checking files already there");
    std::vector<Job> jobs;
    uint64_t total = 0;
    int files = 0;
    std::map<std::string, std::unique_ptr<std::mutex>> fileLocks;
    for (const Depot& d : deps)
      for (const FileEntry& f : d.files) {
        if (cancel) throw Cancelled{};
        const fs::path p = opt.gameDir / fs::path(f.name);
        if (f.flags & 64) {
          fs::create_directories(p, ec);
          continue;
        }
        if (f.flags & 0x80000000u) continue;
        fs::create_directories(p.parent_path(), ec);
        ++files;
        // chunks already right (an earlier, interrupted download) stay
        std::vector<const ChunkInfo*> missing;
        const bool exists = fs::is_regular_file(p, ec) && fs::file_size(p, ec) == f.size;
        if (exists) {
          std::ifstream in(p, std::ios::binary);
          std::vector<uint8_t> buf;
          for (const ChunkInfo& c : f.chunks) {
            buf.resize(c.size);
            in.seekg(std::streamoff(c.offset));
            in.read(reinterpret_cast<char*>(buf.data()), c.size);
            if (!in || steamcrypto::sha1(buf) != c.sha) {
              in.clear();
              missing.push_back(&c);
            }
          }
        } else {
          std::ofstream(p, std::ios::binary | std::ios::trunc);
          fs::resize_file(p, f.size, ec);
          if (ec) throw std::runtime_error("cannot write " + p.string() + " (" + ec.message() + ")");
          for (const ChunkInfo& c : f.chunks) missing.push_back(&c);
        }
        for (const ChunkInfo* c : missing) {
          jobs.push_back({&d, &f, c});
          total += c->size;
        }
        fileLocks[f.name] = std::make_unique<std::mutex>();
      }
    const auto space = fs::space(opt.gameDir, ec);
    if (!ec && space.available < total / 8)  // (files are already allocated at full size; this only catches the worst)
      throw std::runtime_error("not enough free storage for the game");
    set([&](Status& s) {
      s.phase = Phase::Downloading, s.bytesDone = 0, s.bytesTotal = total, s.filesTotal = files, s.filesDone = 0;
      s.message = total ? "downloading Cities in Motion" : "all files are already there";
    });
    LOG_INFO("steam: %zu chunks, %llu bytes to download", jobs.size(), (unsigned long long)total);
    // the biggest files' chunks first spread the work; workers take jobs in order
    std::atomic<size_t> next{0};
    std::atomic<bool> failed{false};
    std::string failure;
    std::mutex failMu;
    auto worker = [&](int id) {
      net::Http http;
      std::vector<uint8_t> raw, plain, data;
      size_t server = size_t(id) % cdns.size();
      while (!failed && !cancel) {
        const size_t i = next++;
        if (i >= jobs.size()) break;
        const Job& j = jobs[i];
        bool ok = false;
        std::string err;
        for (int attempt = 0; attempt < 6 && !ok && !cancel; ++attempt) {
          const std::string url = cdns[server] + "/depot/" + std::to_string(j.depot->id) + "/chunk/" + steamcrypto::hex(j.chunk->sha);
          if (!http.get(url, raw, &err, nullptr, &cancel)) {
            server = (server + 1) % cdns.size();
            std::this_thread::sleep_for(std::chrono::milliseconds(300 * attempt));
            continue;
          }
          if (!steamcrypto::symmetricDecrypt(raw, j.depot->key, plain)) {
            err = "chunk could not be decrypted";
            continue;
          }
          if (plain.size() >= 2 && plain[0] == 'V' && plain[1] == 'Z') ok = unVZip(plain, data);
          else if (plain.size() >= 2 && plain[0] == 'P' && plain[1] == 'K') ok = unzipFirst(plain, data);
          else if (plain.size() >= 4 && std::memcmp(plain.data(), "VSZa", 4) == 0) ok = unVZstd(plain, data, j.chunk->size);
          else err = "unknown chunk format";
          if (!ok && err.empty()) err = "chunk could not be decompressed";
          if (ok && (data.size() != j.chunk->size || steamAdler(data) != j.chunk->crc)) {
            ok = false;
            err = "chunk checksum mismatch";
          }
        }
        if (!ok) {
          if (cancel) break;
          std::lock_guard<std::mutex> lk(failMu);
          if (!failed) failure = j.file->name + ": " + err;
          failed = true;
          break;
        }
        {
          std::lock_guard<std::mutex> lk(*fileLocks[j.file->name]);
          std::FILE* f = std::fopen((opt.gameDir / fs::path(j.file->name)).string().c_str(), "r+b");
          bool wrote = f && fseeko(f, off_t(j.chunk->offset), SEEK_SET) == 0 && std::fwrite(data.data(), 1, data.size(), f) == data.size();
          if (f) std::fclose(f);
          if (!wrote) {
            std::lock_guard<std::mutex> lk2(failMu);
            if (!failed) failure = "cannot write " + j.file->name;
            failed = true;
            break;
          }
        }
        set([&](Status& s) { s.bytesDone += data.size(); });
      }
    };
    std::vector<std::thread> workers;
    for (int i = 0; i < std::max(1, opt.threads); ++i) workers.emplace_back(worker, i);
    for (auto& w : workers) w.join();
    if (cancel) throw Cancelled{};
    if (failed) throw std::runtime_error("download failed: " + failure);
    set([&](Status& s) { s.filesDone = s.filesTotal; });
  }

  void run(int mode, std::string account, std::string password) {
    try {
      phase(Phase::Connecting, "connecting to Steam");
      Cm cm(cancel);
      std::string error;
      if (!cm.connect(error)) throw std::runtime_error("cannot reach Steam: " + error);
      std::string token, name = account;
      if (mode == 0) {
        token = loginPassword(cm, account, password, name);
        password.assign(password.size(), '\0');
      } else if (mode == 1) {
        token = loginQr(cm, name);
      } else if (mode == 2) {
        auto s = load(opt.tokenFile);
        if (!s) throw std::runtime_error("no saved Steam login");
        token = s->token, name = s->account;
      }
      if (mode != 3) {
        if (name.empty()) name = account;
        save(name, token);
        set([&](Status& s) { s.account = name; });
      }
      logon(cm, mode == 3 ? std::string() : name, mode == 3 ? std::string() : token);
      phase(Phase::Preparing, "reading the game's depots");
      KV app = appInfo(cm);
      std::vector<Depot> deps = depots(cm, app);
      if (deps.empty())
        throw std::runtime_error(mode == 3 ? "no depot of this app is free" : "this Steam account does not own Cities in Motion");
      const std::vector<std::string> cdns = cdnServers(cm);
      for (Depot& d : deps) {
        phase(Phase::Preparing, "loading the file list of depot " + std::to_string(d.id));
        manifest(cm, d, cdns);
      }
      cm.close();  // the content servers need no connection manager
      download(deps, cdns);
      std::ofstream(opt.gameDir / ".opencim-steam", std::ios::trunc) << "app=" << opt.appId << "\n";
      phase(Phase::Done, "Cities in Motion is ready");
    } catch (const Cancelled&) {
      phase(Phase::Cancelled, "cancelled");
    } catch (const std::exception& e) {
      LOG_ERROR("steam: %s", e.what());
      set([&](Status& s) { s.phase = Phase::Failed, s.message = e.what(); });
    }
    busy = false;
  }
};

Downloader::Downloader(Options options) : impl_(std::make_unique<Impl>()) { impl_->opt = std::move(options); }

Downloader::~Downloader() {
  impl_->cancel = true;
  if (impl_->thread.joinable()) impl_->thread.join();
}

void Downloader::start(int mode, std::string account, std::string password) {
  if (impl_->thread.joinable()) {
    impl_->cancel = true;
    impl_->thread.join();
  }
  impl_->cancel = false;
  impl_->busy = true;
  {
    std::lock_guard<std::mutex> lk(impl_->mu);
    int seq = impl_->st.sequence + 1;
    impl_->st = Status{};
    impl_->st.sequence = seq;
    impl_->pendingCode.clear();
  }
  impl_->thread = std::thread([this, mode, a = std::move(account), p = std::move(password)]() mutable {
    impl_->run(mode, std::move(a), std::move(p));
  });
}

void Downloader::startPassword(const std::string& account, const std::string& password) { start(0, account, password); }
void Downloader::startQr() { start(1, {}, {}); }
bool Downloader::startSaved() {
  if (!hasSavedLogin(impl_->opt.tokenFile)) return false;
  start(2, {}, {});
  return true;
}
void Downloader::startAnonymous() { start(3, {}, {}); }

void Downloader::submitCode(const std::string& code) {
  std::lock_guard<std::mutex> lk(impl_->mu);
  impl_->pendingCode = code;
  impl_->st.codeWrong = false;
  ++impl_->st.sequence;
}

void Downloader::cancel() { impl_->cancel = true; }

Status Downloader::status() const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  return impl_->st;
}

bool Downloader::running() const { return impl_->busy; }

bool Downloader::hasSavedLogin(const std::filesystem::path& tokenFile, std::string* account) {
  if (tokenFile.empty()) return false;
  auto s = Impl::load(tokenFile);
  if (s && account) *account = s->account;
  return s.has_value();
}

void Downloader::forgetLogin(const std::filesystem::path& tokenFile) {
  std::error_code ec;
  if (!tokenFile.empty()) fs::remove(tokenFile, ec);
}

}  // namespace regs::steam
