// HTTPS + WebSocket over WinHTTP (Schannel TLS, system proxies, no extra DLLs).
#pragma once
#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace aoi {

struct HttpResult {
  int status = 0;        // 0 = transport failure, see error
  std::string body;
  std::string error;
};

class WebSocket {
 public:
  ~WebSocket();
  bool send(const std::string &text);
  // Blocks until a text message arrives or the socket closes (nullopt).
  std::optional<std::string> receive();
  void close();
  bool open() const { return ws_ != nullptr && !closed_; }

 private:
  friend class Http;
  HINTERNET conn_ = nullptr, req_ = nullptr, ws_ = nullptr;
  std::mutex send_mu_;
  std::atomic<bool> closed_{false};
  bool handle_closed_ = false;
};

class Http {
 public:
  Http();
  ~Http();
  HttpResult request(const std::string &method, const std::string &url, const std::string &body = {},
                     int timeout_ms = 15000);
  std::unique_ptr<WebSocket> websocket(const std::string &url, int timeout_ms = 15000, std::string *err = nullptr);

 private:
  // A direct session first; the auto-proxy session only if direct cannot
  // connect. WPAD discovery can stall the first request by ~8 s on networks
  // without a WPAD server, so it is never the first thing tried.
  HINTERNET session(bool proxy);
  HINTERNET direct_ = nullptr, proxied_ = nullptr;
  std::mutex mu_;
  std::atomic<int> prefer_proxy_{-1};  // -1 unknown, 0 direct worked, 1 proxy worked
};

}  // namespace aoi
