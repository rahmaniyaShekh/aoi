#include "crypto.h"

#include <openssl/evp.h>
#include <openssl/sha.h>
#include <zlib.h>

#include <cstring>

#include "util.h"

namespace aoi {

namespace {
constexpr char kPrefix[] = "AOI1:";
constexpr uint8_t kMagic[4] = {'A', 'O', 'I', '1'};
constexpr uint8_t kFlagEnc = 0x01;
constexpr int kSalt = 16, kIv = 12, kTag = 16, kIter = 200000;
}  // namespace

std::string generate_code() {
  const int n = int(kCodeAlphabet.size());
  const int limit = 256 - (256 % n);  // 248: reject the biased tail
  std::string out;
  while (int(out.size()) < kCodeLen) {
    uint8_t b[16];
    random_bytes(b, sizeof b);
    for (uint8_t x : b)
      if (x < limit && int(out.size()) < kCodeLen) out += kCodeAlphabet[x % n];
  }
  return out;
}

std::string normalize_code(std::string_view s) {
  std::string o;
  for (char c : s) {
    char u = (c >= 'a' && c <= 'z') ? char(c - 32) : c;
    if (kCodeAlphabet.find(u) != std::string_view::npos) o += u;
  }
  return o;
}

std::string pretty_code(std::string_view c) {
  if (c.size() > 3) return std::string(c.substr(0, 3)) + "-" + std::string(c.substr(3));
  return std::string(c);
}

bool valid_code(std::string_view c) { return int(c.size()) == kCodeLen && normalize_code(c) == c; }

std::string sha256_hex(std::string_view s) {
  uint8_t h[SHA256_DIGEST_LENGTH];
  SHA256(reinterpret_cast<const uint8_t *>(s.data()), s.size(), h);
  return to_hex(h, sizeof h);
}

std::string b64url_encode(const std::vector<uint8_t> &b) {
  static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::string o;
  o.reserve((b.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < b.size(); i += 3) {
    uint32_t v = uint32_t(b[i]) << 16 | uint32_t(b[i + 1]) << 8 | b[i + 2];
    o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += t[v & 63];
  }
  if (i + 1 == b.size()) {
    uint32_t v = uint32_t(b[i]) << 16;
    o += t[v >> 18]; o += t[(v >> 12) & 63];
  } else if (i + 2 == b.size()) {
    uint32_t v = uint32_t(b[i]) << 16 | uint32_t(b[i + 1]) << 8;
    o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63];
  }
  return o;
}

std::optional<std::vector<uint8_t>> b64url_decode(std::string_view s) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
  };
  std::vector<uint8_t> o;
  uint32_t acc = 0;
  int bits = 0;
  for (char c : s) {
    if (c == '=') break;
    int v = val(c);
    if (v < 0) return std::nullopt;
    acc = acc << 6 | uint32_t(v);
    bits += 6;
    if (bits >= 8) { bits -= 8; o.push_back(uint8_t(acc >> bits)); }
  }
  return o;
}

std::vector<uint8_t> deflate_raw(std::string_view in) {
  z_stream z{};
  // windowBits -15 = raw deflate, identical to CompressionStream('deflate-raw').
  deflateInit2(&z, Z_BEST_COMPRESSION, Z_DEFLATED, -15, 9, Z_DEFAULT_STRATEGY);
  std::vector<uint8_t> out(deflateBound(&z, uLong(in.size())) + 16);
  z.next_in = (Bytef *)in.data();
  z.avail_in = uInt(in.size());
  z.next_out = out.data();
  z.avail_out = uInt(out.size());
  deflate(&z, Z_FINISH);
  out.resize(z.total_out);
  deflateEnd(&z);
  return out;
}

std::optional<std::string> inflate_raw(const uint8_t *p, size_t n, size_t limit) {
  z_stream z{};
  if (inflateInit2(&z, -15) != Z_OK) return std::nullopt;
  std::string out;
  z.next_in = (Bytef *)p;
  z.avail_in = uInt(n);
  char buf[16384];
  int rc;
  do {
    z.next_out = (Bytef *)buf;
    z.avail_out = sizeof buf;
    rc = inflate(&z, Z_NO_FLUSH);
    if (rc != Z_OK && rc != Z_STREAM_END) { inflateEnd(&z); return std::nullopt; }
    out.append(buf, sizeof buf - z.avail_out);
    if (out.size() > limit) { inflateEnd(&z); return std::nullopt; }
  } while (rc != Z_STREAM_END && z.avail_in > 0);
  inflateEnd(&z);
  return out;
}

std::vector<uint8_t> aead_seal(const std::vector<uint8_t> &key, const uint8_t *p, size_t n) {
  std::vector<uint8_t> out(12 + n + 16);
  random_bytes(out.data(), 12);
  EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
  int len = 0;
  EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
  EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
  EVP_EncryptInit_ex(c, nullptr, nullptr, key.data(), out.data());
  EVP_EncryptUpdate(c, out.data() + 12, &len, p, int(n));
  int total = len;
  EVP_EncryptFinal_ex(c, out.data() + 12 + total, &len);
  EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, out.data() + 12 + n);
  EVP_CIPHER_CTX_free(c);
  return out;
}

std::optional<std::vector<uint8_t>> aead_open(const std::vector<uint8_t> &key, const uint8_t *p, size_t n) {
  if (key.size() != 32 || n < 28) return std::nullopt;
  size_t ct = n - 28;
  std::vector<uint8_t> out(ct + 16);
  EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
  int len = 0;
  EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
  EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, 12, nullptr);
  EVP_DecryptInit_ex(c, nullptr, nullptr, key.data(), p);
  EVP_DecryptUpdate(c, out.data(), &len, p + 12, int(ct));
  EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, (void *)(p + 12 + ct));
  int ok = EVP_DecryptFinal_ex(c, out.data() + len, &len);
  EVP_CIPHER_CTX_free(c);
  if (ok != 1) return std::nullopt;
  out.resize(ct);
  return out;
}

static bool derive_key(std::string_view code, const uint8_t *salt, uint8_t key[32]) {
  return PKCS5_PBKDF2_HMAC(code.data(), int(code.size()), salt, kSalt, kIter, EVP_sha256(), 32, key) == 1;
}

std::string seal_blob(std::string_view sdp, std::string_view code) {
  std::vector<uint8_t> body = deflate_raw(sdp);
  std::vector<uint8_t> head(5 + kSalt + kIv);
  memcpy(head.data(), kMagic, 4);
  head[4] = kFlagEnc;
  random_bytes(head.data() + 5, kSalt);
  random_bytes(head.data() + 5 + kSalt, kIv);
  uint8_t key[32];
  derive_key(code, head.data() + 5, key);

  std::vector<uint8_t> ct(body.size() + kTag);
  EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
  int len = 0, total = 0;
  EVP_EncryptInit_ex(c, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
  EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, kIv, nullptr);
  EVP_EncryptInit_ex(c, nullptr, nullptr, key, head.data() + 5 + kSalt);
  EVP_EncryptUpdate(c, nullptr, &len, head.data(), int(head.size()));  // AAD
  EVP_EncryptUpdate(c, ct.data(), &len, body.data(), int(body.size()));
  total = len;
  EVP_EncryptFinal_ex(c, ct.data() + total, &len);
  total += len;
  EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, kTag, ct.data() + total);
  EVP_CIPHER_CTX_free(c);
  ct.resize(size_t(total) + kTag);
  OPENSSL_cleanse(key, sizeof key);

  head.insert(head.end(), ct.begin(), ct.end());
  return std::string(kPrefix) + b64url_encode(head);
}

std::optional<std::string> open_blob(std::string_view blob, std::string_view code, std::string *err) {
  auto fail = [&](const char *why) -> std::optional<std::string> { if (err) *err = why; return std::nullopt; };
  while (!blob.empty() && (blob.back() == ' ' || blob.back() == '\n' || blob.back() == '\r')) blob.remove_suffix(1);
  if (blob.substr(0, sizeof kPrefix - 1) != kPrefix) return fail("not a blob");
  auto raw = b64url_decode(blob.substr(sizeof kPrefix - 1));
  if (!raw || raw->size() < 5 || memcmp(raw->data(), kMagic, 4) != 0) return fail("not a blob");
  const uint8_t *r = raw->data();
  if (!(r[4] & kFlagEnc)) {
    auto s = inflate_raw(r + 5, raw->size() - 5);
    return s ? s : fail("corrupt");
  }
  const size_t headLen = 5 + kSalt + kIv;
  if (raw->size() < headLen + kTag) return fail("corrupt");
  uint8_t key[32];
  derive_key(code, r + 5, key);
  size_t ctLen = raw->size() - headLen - kTag;
  std::vector<uint8_t> plain(ctLen + 16);
  EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
  int len = 0, total = 0;
  EVP_DecryptInit_ex(c, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
  EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, kIv, nullptr);
  EVP_DecryptInit_ex(c, nullptr, nullptr, key, r + 5 + kSalt);
  EVP_DecryptUpdate(c, nullptr, &len, r, int(headLen));
  EVP_DecryptUpdate(c, plain.data(), &len, r + headLen, int(ctLen));
  total = len;
  EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, kTag, (void *)(r + headLen + ctLen));
  int ok = EVP_DecryptFinal_ex(c, plain.data() + total, &len);
  EVP_CIPHER_CTX_free(c);
  OPENSSL_cleanse(key, sizeof key);
  // GCM cannot tell a wrong key from tampering; the wrong code is by far the likelier.
  if (ok != 1) return fail("wrong code");
  total += len;
  auto s = inflate_raw(plain.data(), size_t(total));
  return s ? s : fail("corrupt");
}

}  // namespace aoi
