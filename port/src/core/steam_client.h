#pragma once
// OpenCiM's own small Steam client (owner's request 2026-11-28, docs/android.md): it downloads the player's own copy of
// Cities in Motion where Valve's SteamCMD does not run (Android). Like SteamCMD it signs in to the player's Steam
// account, either with account name and password (then Steam Guard: a confirmation in the Steam app, or a code from
// the app or the e-mail) or with a QR code scanned in the Steam app, fetches the game's Windows depots and the DLC
// depots the account owns from Valve's content servers, and writes the files into the game folder.
//
// Protocol: a WebSocket to a Steam connection manager (protobuf messages, the Authentication service for the login,
// a client logon with the login's refresh token, PICS for the depots and manifests, the depot keys), then the content
// servers over HTTPS (manifests and chunks, AES-encrypted with the depot key, compressed with LZMA or zip). OpenCiM
// keeps the refresh token (tokenFile, the app's private storage) so an update or repair needs no new login; it never
// stores the password.
//
// Everything runs on the downloader's own thread; status() can be read from any thread (the setup screen polls it).
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace regs::steam {

enum class Phase {
  Idle,
  Connecting,      // to Steam
  SigningIn,       // the login is being checked
  NeedCode,        // a Steam Guard code is needed (code: which); submitCode()
  ConfirmInApp,    // waiting for the confirmation in the Steam app (a code may be entered instead when allowCode)
  ShowQr,          // qrUrl is to be shown as a QR code for the Steam app
  Preparing,       // logged in: the game's depots, keys and file lists
  Downloading,     // bytesDone / bytesTotal
  Done,            // the game is in gameDir
  Failed,          // message says why
  Cancelled,
};

enum class CodeKind { None, Email, Authenticator };

struct Status {
  Phase phase = Phase::Idle;
  std::string message;      // English: what happens now, or why it failed
  std::string qrUrl;        // ShowQr
  CodeKind code = CodeKind::None;
  bool allowCode = false;   // ConfirmInApp: an authenticator code works too
  std::string codeHint;     // e.g. the e-mail domain the code went to
  bool codeWrong = false;   // the last code was not accepted
  std::string account;      // the account signed in
  uint64_t bytesDone = 0, bytesTotal = 0;
  int filesDone = 0, filesTotal = 0;
  int sequence = 0;         // increases with every change
};

struct Options {
  std::filesystem::path gameDir;     // where the game's files go (created)
  std::filesystem::path tokenFile;   // the saved login; empty: none is kept
  uint32_t appId = 73010;            // Cities in Motion
  std::string os = "windows";        // which depots (the port reads the Windows version on every system)
  std::string deviceName = "OpenCiM";
  int threads = 8;                   // parallel chunk downloads
  std::vector<uint32_t> onlyDepots;  // tests: just these
};

class Downloader {
 public:
  explicit Downloader(Options options);
  ~Downloader();  // cancels and waits for the thread
  Downloader(const Downloader&) = delete;
  Downloader& operator=(const Downloader&) = delete;

  void startPassword(const std::string& account, const std::string& password);
  void startQr();
  bool startSaved();      // with the saved login; false when there is none
  void startAnonymous();  // tests: Steam's anonymous account (free content only)
  void submitCode(const std::string& code);
  void cancel();
  Status status() const;
  bool running() const;

  static bool hasSavedLogin(const std::filesystem::path& tokenFile, std::string* account = nullptr);
  static void forgetLogin(const std::filesystem::path& tokenFile);

  struct Impl;

 private:
  void start(int mode, std::string account, std::string password);
  std::unique_ptr<Impl> impl_;
};

}  // namespace regs::steam
