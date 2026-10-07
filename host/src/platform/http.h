// HTTPS + WebSocket client. Windows: WinHTTP (Schannel TLS, system proxies,
// no extra DLLs).
#pragma once
#include <memory>
#include <optional>
#include <string>

namespace aoi {

struct HttpResult {
  int status = 0;  // 0 = transport failure, see error
  std::string body;
  std::string error;
};

class WebSocket {
 public:
  virtual ~WebSocket() = default;
  virtual bool send(const std::string &text) = 0;
  virtual bool send_binary(const void *p, size_t n) = 0;
  // Blocks until a message arrives or the socket closes (nullopt).
  virtual std::optional<std::string> receive(bool *binary = nullptr) = 0;
  // Safe from any thread; unblocks a pending receive().
  virtual void close() = 0;
  virtual bool open() const = 0;
};

class Http {
 public:
  Http();
  ~Http();
  Http(const Http &) = delete;
  Http &operator=(const Http &) = delete;
  HttpResult request(const std::string &method, const std::string &url, const std::string &body = {},
                     int timeout_ms = 15000);
  std::unique_ptr<WebSocket> websocket(const std::string &url, int timeout_ms = 15000, std::string *err = nullptr);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace aoi
