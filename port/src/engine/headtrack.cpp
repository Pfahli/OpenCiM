// Head tracking from OpenTrack UDP (engine/headtrack.h, docs/stereo-3d.md).
#include "engine/headtrack.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "core/env.h"
#include "core/log.h"
#include "engine/settings.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using SocketHandle = SOCKET;
static const SocketHandle kNoSocket = INVALID_SOCKET;
static void closeSocket(SocketHandle s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
static const SocketHandle kNoSocket = -1;
static void closeSocket(SocketHandle s) { close(s); }
#endif

namespace regs::engine {

using namespace regs::render;

namespace {

constexpr int kPort = 4242;
constexpr float kViewingCm = 60;  // the head's assumed distance from the screen: an offset this far turns by 45°

struct HeadState {
  bool on = false;
  bool failed = false;  // the port could not be opened (logged once)
  SocketHandle sock = kNoSocket;
  float strength = 1;
  // the newest pose (cm) and when it came; the smoothed offset the camera uses
  float rawX = 0, rawY = 0;
  std::chrono::steady_clock::time_point last{};
  bool havePose = false;
  float x = 0, y = 0;
  bool test = false;
};

HeadState& state() {
  static HeadState s;
  return s;
}

bool openSocket(HeadState& s) {
#ifdef _WIN32
  static bool wsa = false;
  if (!wsa) {
    WSADATA d;
    if (WSAStartup(MAKEWORD(2, 2), &d) != 0) return false;
    wsa = true;
  }
#endif
  s.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s.sock == kNoSocket) return false;
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(kPort);
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // this PC only (no firewall question, nothing from the network)
  if (bind(s.sock, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
    closeSocket(s.sock);
    s.sock = kNoSocket;
    return false;
  }
#ifdef _WIN32
  u_long nb = 1;
  ioctlsocket(s.sock, FIONBIO, &nb);
#else
  fcntl(s.sock, F_SETFL, fcntl(s.sock, F_GETFL, 0) | O_NONBLOCK);
#endif
  return true;
}

double readDouble(const unsigned char* p) {  // little-endian IEEE double, whatever the host
  uint64_t v = 0;
  for (int k = 7; k >= 0; --k) v = (v << 8) | p[k];
  double d;
  std::memcpy(&d, &v, sizeof d);
  return d;
}

}  // namespace

void headTrackFrame(script::VM& vm, float dt) {
  HeadState& s = state();
  bool on = settingNumber(vm, u"regsHeadTracking", 0) > 0;
  if (const char* e = regs::envVar("REGS_HEADTRACK")) on = std::atoi(e) != 0;
  s.strength = std::clamp(settingNumber(vm, u"regsHeadTrackingStrength", 100), 0.0f, 200.0f) / 100.0f;
  if (on != s.on) {
    s.on = on;
    if (on && !s.test) {
      if (openSocket(s)) {
        LOG_INFO("head tracking: listening for OpenTrack UDP on 127.0.0.1:%d", kPort);
        s.failed = false;
      } else if (!s.failed) {
        LOG_WARN("head tracking: cannot listen on 127.0.0.1:%d (another program has the port?)", kPort);
        s.failed = true;
      }
    } else if (!on) {
      if (s.sock != kNoSocket) closeSocket(s.sock);
      s.sock = kNoSocket;
      s.havePose = false;
      LOG_INFO("head tracking: off");
    }
  }
  if (s.on && s.sock == kNoSocket && !s.failed && !s.test) {  // (the port taken at first: try again now and then)
    static float retry = 0;
    if ((retry += dt) > 2) {
      retry = 0;
      if (openSocket(s)) LOG_INFO("head tracking: listening for OpenTrack UDP on 127.0.0.1:%d", kPort);
    }
  }
  const auto now = std::chrono::steady_clock::now();
  if (s.sock != kNoSocket) {
    unsigned char buf[256];
    for (int n = 0; n < 64; ++n) {  // the newest of what came since the last frame
      const int got = int(recv(s.sock, reinterpret_cast<char*>(buf), sizeof buf, 0));
      if (got < 48) break;
      const double px = readDouble(buf), py = readDouble(buf + 8);
      if (std::isfinite(px) && std::isfinite(py)) {
        if (!s.havePose) LOG_INFO("head tracking: receiving");
        s.rawX = float(std::clamp(px, -60.0, 60.0));
        s.rawY = float(std::clamp(py, -40.0, 40.0));
        s.last = now;
        s.havePose = true;
      }
    }
  }
  // no packet for half a second (tracker stopped, head lost): back to the middle
  const bool fresh = s.havePose && (s.test || now - s.last < std::chrono::milliseconds(500));
  const float tx = s.on && fresh ? s.rawX : 0, ty = s.on && fresh ? s.rawY : 0;
  const float k = 1.0f - std::exp(-std::max(dt, 0.0f) * (fresh ? 20.0f : 4.0f));  // light smoothing (the trackers filter)
  s.x += (tx - s.x) * k;
  s.y += (ty - s.y) * k;
}

void headTrackCamera(Vec3& eye, const Vec3& look) {
  const HeadState& s = state();
  if (std::fabs(s.x) < 1e-3f && std::fabs(s.y) < 1e-3f) return;
  Vec3 v = eye - look;
  const float dist = std::sqrt(dot(v, v));
  if (dist < 1e-3f) return;
  // sideways: about the world's up axis, the eye toward the head's side
  const float yaw = std::atan(s.x / kViewingCm) * s.strength;
  const float cy = std::cos(yaw), sy = std::sin(yaw);
  v = {v.x * cy + v.z * sy, v.y, -v.x * sy + v.z * cy};
  // up / down: about the camera's right axis, higher when the head is higher, 15° at most either way and never
  // below the horizon or past straight down
  const Vec3 up{0, 1, 0};
  Vec3 right = cross(up, v);
  const float rl = std::sqrt(dot(right, right));
  if (rl > 1e-4f) {
    right = right * (1.0f / rl);
    const float pitchNow = std::asin(std::clamp(v.y / dist, -1.0f, 1.0f));
    float turn = std::clamp(std::atan(s.y / kViewingCm) * s.strength, -0.26f, 0.26f);
    turn = std::clamp(turn, 0.05f - pitchNow, 1.50f - pitchNow);
    const float c = std::cos(turn), sn = std::sin(turn);
    // about `right` (v is perpendicular to it): v·cos + (v × right)·sin, positive raising the eye
    const Vec3 rv = cross(v, right);
    v = v * c + rv * sn;
  }
  eye = look + v;
}

void headTrackTest(float xCm, float yCm) {
  HeadState& s = state();
  s.test = true;
  s.rawX = xCm;
  s.rawY = yCm;
  s.havePose = true;
  s.last = std::chrono::steady_clock::now();
}

int headTrackState() {
  const HeadState& s = state();
  if (!s.on) return 0;
  return s.havePose && std::chrono::steady_clock::now() - s.last < std::chrono::milliseconds(500) ? 2 : 1;
}

}  // namespace regs::engine
