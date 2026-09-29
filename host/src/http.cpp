#include "http.h"

#include <algorithm>
#include <vector>

#include "util.h"

namespace aoi {

namespace {
struct Url {
  std::wstring host, path;
  INTERNET_PORT port = 443;
  bool tls = true;
};

bool crack(const std::string &url, Url &u) {
  std::wstring w = widen(url);
  URL_COMPONENTS c{};
  c.dwStructSize = sizeof c;
  wchar_t host[256], path[2048];
  c.lpszHostName = host; c.dwHostNameLength = DWORD(std::size(host));
  c.lpszUrlPath = path; c.dwUrlPathLength = DWORD(std::size(path));
  wchar_t extra[2048];
  c.lpszExtraInfo = extra; c.dwExtraInfoLength = DWORD(std::size(extra));
  if (!WinHttpCrackUrl(w.c_str(), DWORD(w.size()), 0, &c)) return false;
  u.host.assign(host, c.dwHostNameLength);
  u.path.assign(path, c.dwUrlPathLength);
  u.path.append(extra, c.dwExtraInfoLength);
  if (u.path.empty()) u.path = L"/";
  u.port = c.nPort;
  u.tls = c.nScheme == INTERNET_SCHEME_HTTPS;
  return true;
}

bool transport_error(DWORD e) {
  return e == ERROR_WINHTTP_CANNOT_CONNECT || e == ERROR_WINHTTP_NAME_NOT_RESOLVED ||
         e == ERROR_WINHTTP_TIMEOUT || e == ERROR_WINHTTP_CONNECTION_ERROR;
}

std::string err_text(DWORD e) {
  switch (e) {
    case ERROR_WINHTTP_CANNOT_CONNECT: return "cannot connect";
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "name not resolved (offline?)";
    case ERROR_WINHTTP_TIMEOUT: return "timed out";
    case ERROR_WINHTTP_CONNECTION_ERROR: return "connection reset";
    case ERROR_WINHTTP_SECURE_FAILURE: return "TLS failure";
    default: return "winhttp error " + std::to_string(e);
  }
}
}  // namespace

Http::Http() = default;
Http::~Http() {
  if (direct_) WinHttpCloseHandle(direct_);
  if (proxied_) WinHttpCloseHandle(proxied_);
}

HINTERNET Http::session(bool proxy) {
  std::lock_guard lk(mu_);
  HINTERNET &h = proxy ? proxied_ : direct_;
  if (!h) {
    std::wstring ua = L"AOI/" + widen(AOI_VERSION) + L" (Windows)";
    h = WinHttpOpen(ua.c_str(), proxy ? WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY : WINHTTP_ACCESS_TYPE_NO_PROXY,
                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (h) {
      DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
      WinHttpSetOption(h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
    }
  }
  return h;
}

HttpResult Http::request(const std::string &method, const std::string &url, const std::string &body, int timeout_ms) {
  HttpResult res;
  Url u;
  if (!crack(url, u)) { res.error = "bad url"; return res; }

  int pref = prefer_proxy_.load();
  // direct, direct again (a fresh connection often lands on the other
  // address), then the system proxy
  bool order[3] = {pref == 1, pref == 1, pref != 1};
  for (int attempt = 0; attempt < 3; ++attempt) {
    bool proxy = order[attempt];
    HINTERNET s = session(proxy);
    if (!s) continue;
    HINTERNET c = WinHttpConnect(s, u.host.c_str(), u.port, 0);
    if (!c) continue;
    HINTERNET r = WinHttpOpenRequest(c, widen(method).c_str(), u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                     WINHTTP_DEFAULT_ACCEPT_TYPES, u.tls ? WINHTTP_FLAG_SECURE : 0);
    if (!r) { WinHttpCloseHandle(c); continue; }
    // Connect capped at 3.5 s: on a network that drops SYNs to one of the
    // server's addresses, WinHTTP moves on instead of waiting out TCP's 21 s.
    WinHttpSetTimeouts(r, timeout_ms, std::min(timeout_ms, 3500), timeout_ms, timeout_ms);
    const wchar_t *hdr = L"Content-Type: application/json\r\nCache-Control: no-store\r\n";
    BOOL ok = WinHttpSendRequest(r, hdr, DWORD(-1), (LPVOID)(body.empty() ? nullptr : body.data()),
                                 DWORD(body.size()), DWORD(body.size()), 0) &&
              WinHttpReceiveResponse(r, nullptr);
    if (!ok) {
      DWORD e = GetLastError();
      res.error = err_text(e);
      WinHttpCloseHandle(r); WinHttpCloseHandle(c);
      if (transport_error(e)) continue;  // try the other route
      return res;
    }
    DWORD status = 0, len = sizeof status;
    WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                        &status, &len, WINHTTP_NO_HEADER_INDEX);
    res.status = int(status);
    for (;;) {
      DWORD avail = 0;
      if (!WinHttpQueryDataAvailable(r, &avail) || avail == 0) break;
      size_t at = res.body.size();
      res.body.resize(at + avail);
      DWORD got = 0;
      if (!WinHttpReadData(r, res.body.data() + at, avail, &got)) { res.body.resize(at); break; }
      res.body.resize(at + got);
      if (res.body.size() > (1 << 20)) break;
    }
    WinHttpCloseHandle(r); WinHttpCloseHandle(c);
    prefer_proxy_ = proxy ? 1 : 0;
    res.error.clear();
    return res;
  }
  return res;
}

std::unique_ptr<WebSocket> Http::websocket(const std::string &url, int timeout_ms, std::string *err) {
  Url u;
  if (!crack(url, u)) { if (err) *err = "bad url"; return nullptr; }
  int pref = prefer_proxy_.load();
  bool order[2] = {pref == 1, pref != 1};
  for (int attempt = 0; attempt < 2; ++attempt) {
    bool proxy = order[attempt];
    HINTERNET s = session(proxy);
    if (!s) continue;
    auto ws = std::make_unique<WebSocket>();
    ws->conn_ = WinHttpConnect(s, u.host.c_str(), u.port, 0);
    if (!ws->conn_) continue;
    ws->req_ = WinHttpOpenRequest(ws->conn_, L"GET", u.path.c_str(), nullptr, WINHTTP_NO_REFERER,
                                  WINHTTP_DEFAULT_ACCEPT_TYPES, u.tls ? WINHTTP_FLAG_SECURE : 0);
    if (!ws->req_) continue;
    WinHttpSetTimeouts(ws->req_, timeout_ms, std::min(timeout_ms, 3500), timeout_ms, 0);  // receive: wait forever
    WinHttpSetOption(ws->req_, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);
    if (!WinHttpSendRequest(ws->req_, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(ws->req_, nullptr)) {
      DWORD e = GetLastError();
      if (err) *err = err_text(e);
      if (transport_error(e)) continue;
      return nullptr;
    }
    DWORD status = 0, len = sizeof status;
    WinHttpQueryHeaders(ws->req_, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
    if (status != 101) { if (err) *err = "upgrade refused: HTTP " + std::to_string(status); return nullptr; }
    ws->ws_ = WinHttpWebSocketCompleteUpgrade(ws->req_, 0);
    if (!ws->ws_) { if (err) *err = err_text(GetLastError()); return nullptr; }
    WinHttpCloseHandle(ws->req_);
    ws->req_ = nullptr;
    prefer_proxy_ = proxy ? 1 : 0;
    return ws;
  }
  if (err && err->empty()) *err = "cannot connect";
  return nullptr;
}

// ---- WebSocket -------------------------------------------------------------------
WebSocket::~WebSocket() {
  close();
  if (req_) WinHttpCloseHandle(req_);
  if (conn_) WinHttpCloseHandle(conn_);
}

bool WebSocket::send(const std::string &text) {
  if (!ws_ || closed_) return false;
  std::lock_guard lk(send_mu_);
  DWORD e = WinHttpWebSocketSend(ws_, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, (PVOID)text.data(),
                                 DWORD(text.size()));
  if (e != NO_ERROR) { closed_ = true; return false; }
  return true;
}

std::optional<std::string> WebSocket::receive() {
  std::string msg;
  std::vector<char> buf(8192);
  HINTERNET h = ws_;
  while (h && !closed_) {
    DWORD got = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
    DWORD e = WinHttpWebSocketReceive(h, buf.data(), DWORD(buf.size()), &got, &type);
    if (e != NO_ERROR || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) { closed_ = true; break; }
    msg.append(buf.data(), got);
    if (msg.size() > (1 << 20)) { closed_ = true; break; }
    if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE)
      return msg;
  }
  return std::nullopt;
}

void WebSocket::close() {
  if (!ws_) return;
  bool was = closed_.exchange(true);
  if (!was) WinHttpWebSocketShutdown(ws_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
  // Closing the handle unblocks a receive pending on another thread. The
  // handle value stays readable (never reused by us) so that thread exits cleanly.
  std::lock_guard lk(send_mu_);
  if (!handle_closed_) { handle_closed_ = true; WinHttpCloseHandle(ws_); }
}

}  // namespace aoi
