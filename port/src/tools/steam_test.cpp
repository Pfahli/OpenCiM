// steam-test: the Steam client (core/steam_client.h) from a terminal, for tests outside the Android app.
//   steam-test anon <dir> [<appid> [<depot>...]]   Steam's anonymous account: free content (e.g. app 1007)
//   steam-test qr <dir> [<token file>]             QR login, printed in the terminal, then Cities in Motion
//   steam-test password <dir> <account> [<token file>]  password from stdin (not echoed), then Cities in Motion
//   steam-test saved <dir> <token file>            the saved login
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>

#include "core/steam_client.h"
#include "qrcodegen.hpp"

using namespace regs::steam;

namespace {

const char* phaseName(Phase p) {
  switch (p) {
    case Phase::Idle: return "idle";
    case Phase::Connecting: return "connecting";
    case Phase::SigningIn: return "signing in";
    case Phase::NeedCode: return "need code";
    case Phase::ConfirmInApp: return "confirm in app";
    case Phase::ShowQr: return "show QR";
    case Phase::Preparing: return "preparing";
    case Phase::Downloading: return "downloading";
    case Phase::Done: return "done";
    case Phase::Failed: return "failed";
    case Phase::Cancelled: return "cancelled";
  }
  return "?";
}

void printQr(const std::string& url) {
  using qrcodegen::QrCode;
  const QrCode qr = QrCode::encodeText(url.c_str(), QrCode::Ecc::LOW);
  const int n = qr.getSize();
  for (int y = -2; y < n + 2; y += 2) {
    std::string line;
    for (int x = -2; x < n + 2; ++x) {
      const bool top = qr.getModule(x, y), bottom = qr.getModule(x, y + 1);
      line += top && bottom ? " " : top ? "\xe2\x96\x84" : bottom ? "\xe2\x96\x80" : "\xe2\x96\x88";
    }
    std::printf("%s\n", line.c_str());
  }
  std::printf("%s\n", url.c_str());
}

std::string readHidden() {
  termios old{};
  const bool tty = tcgetattr(STDIN_FILENO, &old) == 0;
  if (tty) {
    termios t = old;
    t.c_lflag &= ~tcflag_t(ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &t);
  }
  std::string s;
  std::getline(std::cin, s);
  if (tty) tcsetattr(STDIN_FILENO, TCSANOW, &old);
  std::printf("\n");
  return s;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: steam-test anon|qr|password|saved <dir> ...\n");
    return 2;
  }
  const std::string mode = argv[1];
  Options o;
  o.gameDir = argv[2];
  if (mode == "anon") {
    if (argc > 3) o.appId = uint32_t(std::atoi(argv[3]));
    for (int i = 4; i < argc; ++i) o.onlyDepots.push_back(uint32_t(std::atoi(argv[i])));
  } else if (mode == "password") {
    if (argc < 4) return 2;
    if (argc > 4) o.tokenFile = argv[4];
  } else if (argc > 3) {
    o.tokenFile = argv[3];
  }
  Downloader d(o);
  if (mode == "anon") d.startAnonymous();
  else if (mode == "qr") d.startQr();
  else if (mode == "saved") {
    if (!d.startSaved()) {
      std::fprintf(stderr, "no saved login in %s\n", o.tokenFile.string().c_str());
      return 1;
    }
  } else if (mode == "password") {
    std::printf("Steam password for %s: ", argv[3]);
    std::fflush(stdout);
    d.startPassword(argv[3], readHidden());
  } else return 2;
  int seq = -1;
  std::string lastQr;
  Phase lastPhase = Phase::Idle;
  auto lastPrint = std::chrono::steady_clock::now();
  while (true) {
    Status s = d.status();
    if (s.sequence != seq) {
      seq = s.sequence;
      if (s.phase != lastPhase || s.phase != Phase::Downloading ||
          std::chrono::steady_clock::now() - lastPrint > std::chrono::seconds(2)) {
        lastPrint = std::chrono::steady_clock::now();
        if (s.phase == Phase::Downloading)
          std::printf("[%s] %s: %.1f / %.1f MB, %d files\n", phaseName(s.phase), s.message.c_str(), s.bytesDone / 1048576.0,
                      s.bytesTotal / 1048576.0, s.filesTotal);
        else
          std::printf("[%s] %s\n", phaseName(s.phase), s.message.c_str());
      }
      lastPhase = s.phase;
      if (s.phase == Phase::ShowQr && s.qrUrl != lastQr) printQr(lastQr = s.qrUrl);
      if (s.phase == Phase::NeedCode || (s.phase == Phase::ConfirmInApp && s.allowCode)) {
        std::printf("Steam Guard code%s: ", s.phase == Phase::ConfirmInApp ? " (or confirm in the app; empty line waits)" : "");
        std::fflush(stdout);
        std::string code;
        std::getline(std::cin, code);
        if (!code.empty()) d.submitCode(code);
      }
    }
    if (s.phase == Phase::Done) return 0;
    if (s.phase == Phase::Failed || s.phase == Phase::Cancelled) return 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}
