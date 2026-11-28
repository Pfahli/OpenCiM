// The Android side of the native code (docs/android.md): the program's output into logcat (Android drops stdout and
// stderr), and the JNI functions of the setup screen (android/java/.../SteamBridge.java): the Steam client
// (core/steam_client.h), the game folder check and the QR code's modules.
#include <android/log.h>
#include <jni.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <thread>

#include "core/game_find.h"
#include "core/steam_client.h"
#include "qrcodegen.hpp"

namespace regs {

// stdout and stderr through a pipe into logcat (tag OpenCiM), line by line
void androidLogToLogcat() {
  static bool done = false;
  if (done) return;
  done = true;
  int fds[2];
  if (pipe(fds) != 0) return;
  dup2(fds[1], 1);
  dup2(fds[1], 2);
  std::thread([fd = fds[0]] {
    char buf[2048];
    std::string line;
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
      for (ssize_t i = 0; i < n; ++i) {
        if (buf[i] == '\n') {
          __android_log_write(ANDROID_LOG_INFO, "OpenCiM", line.c_str());
          line.clear();
        } else {
          line += buf[i];
        }
      }
    }
  }).detach();
}

}  // namespace regs

namespace {

std::string str(JNIEnv* env, jstring s) {
  if (!s) return {};
  const char* c = env->GetStringUTFChars(s, nullptr);
  std::string r = c ? c : "";
  if (c) env->ReleaseStringUTFChars(s, c);
  return r;
}

// Java's modified UTF-8 takes no 4-byte sequences: strings for Java go as UTF-16
jstring jstr(JNIEnv* env, const std::string& s) {
  std::u16string u;
  for (size_t i = 0; i < s.size();) {
    uint32_t c = uint8_t(s[i]);
    int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    if (n > 1) {
      c &= 0x3F >> (n - 1);
      for (int k = 1; k < n && i + k < s.size(); ++k) c = c << 6 | (uint8_t(s[i + k]) & 0x3F);
    }
    i += n;
    if (c >= 0x10000) {
      c -= 0x10000;
      u += char16_t(0xD800 + (c >> 10));
      u += char16_t(0xDC00 + (c & 0x3FF));
    } else {
      u += char16_t(c);
    }
  }
  return env->NewString(reinterpret_cast<const jchar*>(u.data()), jsize(u.size()));
}

regs::steam::Downloader* dl(jlong h) { return reinterpret_cast<regs::steam::Downloader*>(h); }

const char* phaseName(regs::steam::Phase p) {
  using P = regs::steam::Phase;
  switch (p) {
    case P::Idle: return "idle";
    case P::Connecting: return "connecting";
    case P::SigningIn: return "signing_in";
    case P::NeedCode: return "need_code";
    case P::ConfirmInApp: return "confirm_in_app";
    case P::ShowQr: return "show_qr";
    case P::Preparing: return "preparing";
    case P::Downloading: return "downloading";
    case P::Done: return "done";
    case P::Failed: return "failed";
    case P::Cancelled: return "cancelled";
  }
  return "idle";
}

std::string oneLine(std::string s) {
  for (char& c : s)
    if (c == '\n' || c == '\r') c = ' ';
  return s;
}

}  // namespace

#define JNI(ret, name) extern "C" JNIEXPORT ret JNICALL Java_io_github_pfahli_opencim_SteamBridge_##name

JNI(jlong, nCreate)(JNIEnv* env, jclass, jstring gameDir, jstring tokenFile) {
  regs::androidLogToLogcat();
  regs::steam::Options o;
  o.gameDir = str(env, gameDir);
  o.tokenFile = str(env, tokenFile);
  o.deviceName = "OpenCiM (Android)";
  return reinterpret_cast<jlong>(new regs::steam::Downloader(o));
}

JNI(void, nDestroy)(JNIEnv*, jclass, jlong h) { delete dl(h); }

JNI(void, nStartPassword)(JNIEnv* env, jclass, jlong h, jstring account, jstring password) {
  dl(h)->startPassword(str(env, account), str(env, password));
}

JNI(void, nStartQr)(JNIEnv*, jclass, jlong h) { dl(h)->startQr(); }

JNI(jboolean, nStartSaved)(JNIEnv*, jclass, jlong h) { return dl(h)->startSaved(); }

JNI(void, nSubmitCode)(JNIEnv* env, jclass, jlong h, jstring code) { dl(h)->submitCode(str(env, code)); }

JNI(void, nCancel)(JNIEnv*, jclass, jlong h) { dl(h)->cancel(); }

// the status as "key=value" lines (SteamBridge.Status parses them)
JNI(jstring, nStatus)(JNIEnv* env, jclass, jlong h) {
  const regs::steam::Status s = dl(h)->status();
  std::string r;
  r += std::string("phase=") + phaseName(s.phase) + "\n";
  r += "message=" + oneLine(s.message) + "\n";
  r += "qr=" + oneLine(s.qrUrl) + "\n";
  r += std::string("code=") + (s.code == regs::steam::CodeKind::Email ? "email" : s.code == regs::steam::CodeKind::Authenticator ? "app" : "") + "\n";
  r += std::string("allowCode=") + (s.allowCode ? "1" : "0") + "\n";
  r += "hint=" + oneLine(s.codeHint) + "\n";
  r += std::string("wrong=") + (s.codeWrong ? "1" : "0") + "\n";
  r += "account=" + oneLine(s.account) + "\n";
  r += "done=" + std::to_string(s.bytesDone) + "\n";
  r += "total=" + std::to_string(s.bytesTotal) + "\n";
  r += "files=" + std::to_string(s.filesTotal) + "\n";
  r += "seq=" + std::to_string(s.sequence) + "\n";
  return jstr(env, r);
}

JNI(jstring, nSavedAccount)(JNIEnv* env, jclass, jstring tokenFile) {
  std::string account;
  if (!regs::steam::Downloader::hasSavedLogin(str(env, tokenFile), &account)) return nullptr;
  return jstr(env, account);
}

JNI(void, nForgetLogin)(JNIEnv* env, jclass, jstring tokenFile) { regs::steam::Downloader::forgetLogin(str(env, tokenFile)); }

// "" when the folder holds the game, else the reason (core/game_find.h)
JNI(jstring, nCheckGame)(JNIEnv* env, jclass, jstring dir) {
  regs::androidLogToLogcat();
  const std::filesystem::path p = regs::gameDataFolder(std::filesystem::path(str(env, dir)));
  const regs::GameCheck c = regs::checkGameFolder(p);
  return jstr(env, c.ok ? std::string() : regs::gameFolderMessage(p, c));
}

// the QR code of text: size*size bytes, 1 for a dark module (the size is the square root of the length)
JNI(jbyteArray, nQr)(JNIEnv* env, jclass, jstring text) {
  using qrcodegen::QrCode;
  const QrCode qr = QrCode::encodeText(str(env, text).c_str(), QrCode::Ecc::MEDIUM);
  const int n = qr.getSize();
  std::string m(size_t(n) * size_t(n), '\0');
  for (int y = 0; y < n; ++y)
    for (int x = 0; x < n; ++x) m[size_t(y) * size_t(n) + size_t(x)] = qr.getModule(x, y) ? 1 : 0;
  jbyteArray a = env->NewByteArray(jsize(m.size()));
  env->SetByteArrayRegion(a, 0, jsize(m.size()), reinterpret_cast<const jbyte*>(m.data()));
  return a;
}
