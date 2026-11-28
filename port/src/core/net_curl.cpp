// HTTPS and WebSocket over libcurl (core/net_curl.h).
#include "core/net_curl.h"

#include <curl/curl.h>
#include <poll.h>

#include <chrono>
#include <filesystem>
#include <mutex>

namespace regs::net {
namespace {

void globalInit() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// the certificate store: on Android the system's, which moved into the Conscrypt APEX with Android 14 (the old folder
// stays, but is no longer updated)
void tls(CURL* c) {
#if defined(__ANDROID__)
  std::error_code ec;
  if (std::filesystem::is_directory("/apex/com.android.conscrypt/cacerts", ec))
    curl_easy_setopt(c, CURLOPT_CAPATH, "/apex/com.android.conscrypt/cacerts");
  else
    curl_easy_setopt(c, CURLOPT_CAPATH, "/system/etc/security/cacerts");
#else
  (void)c;
#endif
}

size_t sink(char* p, size_t size, size_t n, void* user) {
  auto* out = static_cast<std::vector<uint8_t>*>(user);
  out->insert(out->end(), p, p + size * n);
  return size * n;
}

int abortCheck(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  auto* cancel = static_cast<const std::atomic<bool>*>(user);
  return cancel && cancel->load() ? 1 : 0;
}

}  // namespace

Http::Http() {
  globalInit();
  curl_ = curl_easy_init();
}

Http::~Http() {
  if (curl_) curl_easy_cleanup(static_cast<CURL*>(curl_));
}

bool Http::get(const std::string& url, std::vector<uint8_t>& out, std::string* error, long* status,
               const std::atomic<bool>* cancel, long timeoutSeconds) {
  out.clear();
  if (status) *status = 0;
  CURL* c = static_cast<CURL*>(curl_);
  if (!c) {
    if (error) *error = "curl_easy_init failed";
    return false;
  }
  curl_easy_reset(c);
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(c, CURLOPT_USERAGENT, "Valve/Steam HTTP Client 1.0");
  curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
  curl_easy_setopt(c, CURLOPT_WRITEDATA, &out);
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
  curl_easy_setopt(c, CURLOPT_TIMEOUT, timeoutSeconds);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
  if (cancel) {
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, abortCheck);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, const_cast<std::atomic<bool>*>(cancel));
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
  }
  tls(c);
  CURLcode rc = curl_easy_perform(c);
  long code = 0;
  curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
  if (status) *status = code;
  if (rc != CURLE_OK) {
    if (error) *error = curl_easy_strerror(rc);
    return false;
  }
  if (code != 200) {
    if (error) *error = "HTTP " + std::to_string(code);
    return false;
  }
  return true;
}

WebSocket::WebSocket() { globalInit(); }

WebSocket::~WebSocket() { close(); }

bool WebSocket::connect(const std::string& url, std::string* error) {
  close();
  CURL* c = curl_easy_init();
  if (!c) {
    if (error) *error = "curl_easy_init failed";
    return false;
  }
  curl_easy_setopt(c, CURLOPT_URL, url.c_str());
  curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 2L);  // the WebSocket upgrade, then curl_ws_send / curl_ws_recv
  curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 20L);
  curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
  tls(c);
  CURLcode rc = curl_easy_perform(c);
  if (rc != CURLE_OK) {
    if (error) *error = std::string("WebSocket: ") + curl_easy_strerror(rc);
    curl_easy_cleanup(c);
    return false;
  }
  curl_ = c;
  return true;
}

namespace {
bool waitSocket(CURL* c, bool write, int timeoutMs) {
  curl_socket_t s = CURL_SOCKET_BAD;
  curl_easy_getinfo(c, CURLINFO_ACTIVESOCKET, &s);
  if (s == CURL_SOCKET_BAD) return false;
  pollfd p{int(s), short(write ? POLLOUT : POLLIN), 0};
  return ::poll(&p, 1, timeoutMs) > 0;
}
}  // namespace

bool WebSocket::send(std::span<const uint8_t> message, std::string* error) {
  CURL* c = static_cast<CURL*>(curl_);
  if (!c) {
    if (error) *error = "not connected";
    return false;
  }
  size_t done = 0;
  while (true) {
    size_t sent = 0;
    // a partial send continues the same frame: curl_ws_send takes the rest with the frame's full size as fragsize 0
    CURLcode rc = curl_ws_send(c, message.data() + done, message.size() - done, &sent, 0, CURLWS_BINARY);
    done += sent;
    if (rc == CURLE_OK && done >= message.size()) return true;
    if (rc == CURLE_OK || rc == CURLE_AGAIN) {
      if (!waitSocket(c, true, 10000)) {
        if (error) *error = "WebSocket: send timed out";
        return false;
      }
      continue;
    }
    if (error) *error = std::string("WebSocket send: ") + curl_easy_strerror(rc);
    return false;
  }
}

int WebSocket::receive(std::vector<uint8_t>& out, int timeoutMs, std::string* error) {
  CURL* c = static_cast<CURL*>(curl_);
  if (!c) {
    if (error) *error = "not connected";
    return -1;
  }
  out.clear();
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  uint8_t buf[64 * 1024];
  bool started = false;
  while (true) {
    size_t got = 0;
    const struct curl_ws_frame* meta = nullptr;
    CURLcode rc = curl_ws_recv(c, buf, sizeof buf, &got, &meta);
    if (rc == CURLE_AGAIN) {
      int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(until - std::chrono::steady_clock::now()).count());
      if (started) left = std::max(left, 5000);  // the rest of a message that began arrives soon
      if (left <= 0) return 0;
      waitSocket(c, false, left);
      if (!started && std::chrono::steady_clock::now() >= until) return 0;
      continue;
    }
    if (rc != CURLE_OK) {
      if (error) *error = std::string("WebSocket receive: ") + curl_easy_strerror(rc);
      return -1;
    }
    if (!meta) continue;
    if (meta->flags & CURLWS_CLOSE) {
      if (error) *error = "the server closed the connection";
      return -1;
    }
    if (meta->flags & (CURLWS_PING | CURLWS_PONG)) continue;  // curl answers pings itself
    started = true;
    out.insert(out.end(), buf, buf + got);
    if (meta->bytesleft == 0 && !(meta->flags & CURLWS_CONT)) return 1;
  }
}

void WebSocket::close() {
  if (CURL* c = static_cast<CURL*>(curl_)) {
    size_t sent = 0;
    curl_ws_send(c, "", 0, &sent, 0, CURLWS_CLOSE);
    curl_easy_cleanup(c);
  }
  curl_ = nullptr;
}

}  // namespace regs::net
