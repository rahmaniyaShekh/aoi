// Join codes and the sealed signalling blob.
//
// Byte-identical to the browser implementation in the listener page:
//   "AOI1:" + base64url( magic "AOI1" | flags | salt[16] | iv[12] | AES-256-GCM(deflate-raw(SDP)) | tag[16] )
//   key = PBKDF2-HMAC-SHA256(code, salt, 200000), AAD = magic | flags | salt | iv
#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aoi {

constexpr std::string_view kCodeAlphabet = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";  // 31, no 0/O/1/I/L
constexpr int kCodeLen = 6;

std::string generate_code();                       // CSPRNG, rejection sampled (no modulo bias)
std::string normalize_code(std::string_view s);    // upper-case, alphabet only
std::string pretty_code(std::string_view c);       // K7Q4MX -> K7Q-4MX
bool valid_code(std::string_view c);

std::string sha256_hex(std::string_view s);
std::string b64url_encode(const std::vector<uint8_t> &b);
std::optional<std::vector<uint8_t>> b64url_decode(std::string_view s);
std::vector<uint8_t> deflate_raw(std::string_view in);
std::optional<std::string> inflate_raw(const uint8_t *p, size_t n, size_t limit = 1 << 20);

std::string seal_blob(std::string_view sdp, std::string_view code);
// Returns nullopt with *err = "wrong code" / "not a blob" / "corrupt".
std::optional<std::string> open_blob(std::string_view blob, std::string_view code, std::string *err = nullptr);

}  // namespace aoi
