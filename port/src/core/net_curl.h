#pragma once
// HTTPS and WebSocket over libcurl (8.11 or newer, with WebSocket support): the Steam client's connection to Valve's
// connection managers and content servers (core/steam_client.h). Built where that curl is available: Android (curl on
// mbedTLS, android/Dockerfile) and the Android build image's host tests.
#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace regs::net {

// One HTTP client (a curl handle with its connection cache): GET into out. Status is the HTTP status (0 when no
// answer); false on a transport error or a status other than 200 (error says which). cancel, when set, aborts.
class Http {
 public:
  Http();
  ~Http();
  Http(const Http&) = delete;
  Http& operator=(const Http&) = delete;
  bool get(const std::string& url, std::vector<uint8_t>& out, std::string* error = nullptr, long* status = nullptr,
           const std::atomic<bool>* cancel = nullptr, long timeoutSeconds = 60);

 private:
  void* curl_ = nullptr;
};

// A WebSocket client for binary messages (whole messages; fragments are joined).
class WebSocket {
 public:
  WebSocket();
  ~WebSocket();
  WebSocket(const WebSocket&) = delete;
  WebSocket& operator=(const WebSocket&) = delete;
  bool connect(const std::string& url, std::string* error);
  bool send(std::span<const uint8_t> message, std::string* error);
  // Waits up to timeoutMs for the next message. 1: a message in out; 0: none in time; -1: closed or failed (error).
  int receive(std::vector<uint8_t>& out, int timeoutMs, std::string* error);
  void close();
  bool open() const { return curl_ != nullptr; }

 private:
  void* curl_ = nullptr;
};

}  // namespace regs::net
