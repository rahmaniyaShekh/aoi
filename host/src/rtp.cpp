#include "rtp.h"

#include <cstring>

#include "util.h"

namespace aoi {

bool parse_rtp(const uint8_t *p, size_t n, RtpPacketInfo &o, int ext_id) {
  if (n < kRtpHeader || (p[0] >> 6) != 2) return false;
  bool padding = p[0] & 0x20, ext = p[0] & 0x10;
  int cc = p[0] & 0x0F;
  o.marker = p[1] & 0x80;
  o.pt = p[1] & 0x7F;
  o.seq = be16(p + 2);
  o.ts = be32(p + 4);
  o.ssrc = be32(p + 8);
  o.twcc_seq.reset();
  size_t off = kRtpHeader + size_t(cc) * 4;
  if (off > n) return false;
  if (ext) {
    if (off + 4 > n) return false;
    uint16_t profile = be16(p + off);
    size_t words = be16(p + off + 2);
    size_t end = off + 4 + words * 4;
    if (end > n) return false;
    size_t q = off + 4;
    if (profile == 0xBEDE) {  // one-byte header elements
      while (q < end) {
        uint8_t b = p[q];
        if (b == 0) { ++q; continue; }
        int id = b >> 4, len = (b & 0x0F) + 1;
        if (id == 15) break;
        if (q + 1 + size_t(len) > end) break;
        if (id == ext_id && len == 2) o.twcc_seq = be16(p + q + 1);
        q += 1 + size_t(len);
      }
    } else if ((profile & 0xFFF0) == 0x1000) {  // two-byte header elements
      while (q + 2 <= end) {
        int id = p[q], len = p[q + 1];
        if (id == 0) { ++q; continue; }
        if (q + 2 + size_t(len) > end) break;
        if (id == ext_id && len == 2) o.twcc_seq = be16(p + q + 2);
        q += 2 + size_t(len);
      }
    }
    off = end;
  }
  size_t pad = padding && n > off ? p[n - 1] : 0;
  if (off + pad > n) return false;
  o.payload = p + off;
  o.payload_len = n - off - pad;
  return true;
}

std::vector<uint8_t> build_rtp(uint8_t pt, uint16_t seq, uint32_t ts, uint32_t ssrc, bool marker, uint16_t tseq,
                               const uint8_t *payload, size_t len) {
  std::vector<uint8_t> pkt(kRtpHeader + 8 + len);
  uint8_t *p = pkt.data();
  p[0] = 0x90;  // V=2, X=1
  p[1] = uint8_t((marker ? 0x80 : 0) | (pt & 0x7F));
  put_be16(p + 2, seq);
  put_be32(p + 4, ts);
  put_be32(p + 8, ssrc);
  put_be16(p + 12, 0xBEDE);
  put_be16(p + 14, 1);                   // one 32-bit word of extension
  p[16] = uint8_t(kExtTwcc << 4 | 1);    // id, len-1 = 1 (two bytes)
  put_be16(p + 17, tseq);
  p[19] = 0;
  if (len) memcpy(p + 20, payload, len);
  return pkt;
}

void set_twcc_seq(std::vector<uint8_t> &pkt, uint16_t tseq) {
  if (pkt.size() >= 20) put_be16(pkt.data() + 17, tseq);
}

std::vector<uint8_t> build_red(const std::vector<const Frame *> &red, const Frame &primary, uint8_t block_pt) {
  std::vector<uint8_t> out;
  std::vector<const Frame *> used;
  for (const Frame *f : red) {
    uint32_t off = primary.ts - f->ts;
    if (off == 0 || off > 0x3FFF || f->data.size() > 0x3FF || f->data.empty()) continue;
    used.push_back(f);
  }
  out.reserve(used.size() * 4 + 1 + primary.data.size() + 256);
  for (const Frame *f : used) {
    uint32_t off = primary.ts - f->ts;
    out.push_back(uint8_t(0x80 | block_pt));
    uint32_t v = (off << 10) | uint32_t(f->data.size());
    out.push_back(uint8_t(v >> 16)); out.push_back(uint8_t(v >> 8)); out.push_back(uint8_t(v));
  }
  out.push_back(block_pt);
  for (const Frame *f : used) out.insert(out.end(), f->data.begin(), f->data.end());
  out.insert(out.end(), primary.data.begin(), primary.data.end());
  return out;
}

bool parse_red(const uint8_t *p, size_t n, uint32_t primary_ts, std::vector<RedBlock> &out) {
  out.clear();
  struct H { uint8_t pt; uint32_t off, len; };
  std::vector<H> hs;
  size_t q = 0;
  for (;;) {
    if (q >= n) return false;
    if (p[q] & 0x80) {
      if (q + 4 > n) return false;
      uint32_t v = be24(p + q + 1);
      hs.push_back({uint8_t(p[q] & 0x7F), v >> 10, v & 0x3FF});
      q += 4;
    } else {
      hs.push_back({uint8_t(p[q] & 0x7F), 0, 0});
      ++q;
      break;
    }
    if (hs.size() > 16) return false;
  }
  for (size_t i = 0; i < hs.size(); ++i) {
    size_t len = i + 1 == hs.size() ? n - q : hs[i].len;
    if (q + len > n) return false;
    out.push_back({primary_ts - hs[i].off, p + q, len, hs[i].pt});
    q += len;
  }
  return true;
}

// ---- RTCP ------------------------------------------------------------------------------------
bool is_rtcp(const uint8_t *p, size_t n) { return n >= 8 && (p[0] >> 6) == 2 && p[1] >= 192 && p[1] <= 223; }

static void parse_blocks(const uint8_t *p, int count, size_t avail, uint32_t our, RtcpEvents &ev) {
  for (int i = 0; i < count && avail >= 24; ++i, p += 24, avail -= 24) {
    ReportBlock b;
    b.ssrc = be32(p);
    b.fraction_lost = p[4];
    int32_t cl = int32_t(be24(p + 5));
    if (cl & 0x800000) cl |= int32_t(0xFF000000);
    b.cumulative_lost = cl;
    b.highest_seq = be32(p + 8);
    b.jitter = be32(p + 12);
    b.lsr = be32(p + 16);
    b.dlsr = be32(p + 20);
    if (b.ssrc == our) ev.reports.push_back(b);
  }
}

bool parse_twcc(const uint8_t *f, size_t n, std::vector<TwccResult> &out) {
  if (n < 8) return false;
  uint16_t base = be16(f), count = be16(f + 2);
  int32_t ref = int32_t(be24(f + 4));
  if (ref & 0x800000) ref |= int32_t(0xFF000000);
  size_t q = 8;
  std::vector<uint8_t> sym;
  sym.reserve(count);
  while (sym.size() < count) {
    if (q + 2 > n) return false;
    uint16_t c = be16(f + q);
    q += 2;
    if (!(c & 0x8000)) {
      uint8_t s = (c >> 13) & 3;
      int run = c & 0x1FFF;
      for (int i = 0; i < run && sym.size() < count; ++i) sym.push_back(s);
    } else if (!(c & 0x4000)) {
      for (int i = 0; i < 14 && sym.size() < count; ++i) sym.push_back((c >> (13 - i)) & 1);
    } else {
      for (int i = 0; i < 7 && sym.size() < count; ++i) sym.push_back((c >> (12 - 2 * i)) & 3);
    }
  }
  int64_t t = int64_t(ref) * 64000;
  for (size_t i = 0; i < sym.size(); ++i) {
    uint16_t seq = uint16_t(base + i);
    if (sym[i] == 1) {
      if (q + 1 > n) return false;
      t += int64_t(f[q]) * 250;
      q += 1;
      out.push_back({seq, true, t});
    } else if (sym[i] == 2) {
      if (q + 2 > n) return false;
      t += int64_t(int16_t(be16(f + q))) * 250;
      q += 2;
      out.push_back({seq, true, t});
    } else {
      out.push_back({seq, false, 0});
    }
  }
  return true;
}

void parse_rtcp(const uint8_t *p, size_t n, uint32_t our, RtcpEvents &ev) {
  size_t off = 0;
  while (off + 4 <= n) {
    const uint8_t *h = p + off;
    if ((h[0] >> 6) != 2) break;
    int fmt = h[0] & 0x1F, pt = h[1];
    size_t len = (size_t(be16(h + 2)) + 1) * 4;
    if (off + len > n) break;
    const uint8_t *body = h + 4;
    size_t blen = len - 4;
    switch (pt) {
      case 200:
        if (blen >= 24) {
          ev.has_sr = true;
          ev.sr_ssrc = be32(body);
          ev.sr_ntp = uint64_t(be32(body + 4)) << 32 | be32(body + 8);
          parse_blocks(body + 24, fmt, blen - 24, our, ev);
        }
        break;
      case 201:
        if (blen >= 4) parse_blocks(body + 4, fmt, blen - 4, our, ev);
        break;
      case 203: ev.bye++; break;
      case 205:
        if (blen >= 8) {
          uint32_t media = be32(body + 4);
          if (fmt == 1 && media == our) {
            for (size_t q = 8; q + 4 <= blen; q += 4) {
              uint16_t pid = be16(body + q), blp = be16(body + q + 2);
              ev.nacks.push_back(pid);
              for (int b = 0; b < 16; ++b)
                if (blp & (1 << b)) ev.nacks.push_back(uint16_t(pid + b + 1));
            }
          } else if (fmt == 15) {
            parse_twcc(body + 8, blen - 8, ev.twcc);
          }
        }
        break;
      case 206:
        if (fmt == 15 && blen >= 16 && memcmp(body + 8, "REMB", 4) == 0) {
          uint8_t e = body[13] >> 2;
          uint32_t mant = (uint32_t(body[13] & 3) << 16) | be16(body + 14);
          ev.remb_bps = uint32_t(std::min<uint64_t>(uint64_t(mant) << e, 0xFFFFFFFFull));
        }
        break;
      default: break;
    }
    off += len;
  }
}

std::vector<uint8_t> build_sr(uint32_t ssrc, uint64_t ntp, uint32_t rtp_ts, uint32_t packets, uint32_t octets) {
  std::vector<uint8_t> b(28);
  b[0] = 0x80; b[1] = 200; put_be16(&b[2], 6);
  put_be32(&b[4], ssrc);
  put_be32(&b[8], uint32_t(ntp >> 32)); put_be32(&b[12], uint32_t(ntp));
  put_be32(&b[16], rtp_ts); put_be32(&b[20], packets); put_be32(&b[24], octets);
  return b;
}

std::vector<uint8_t> build_rr(uint32_t our, const ReportBlock &rb) {
  std::vector<uint8_t> b(32);
  b[0] = 0x81; b[1] = 201; put_be16(&b[2], 7);
  put_be32(&b[4], our);
  uint8_t *p = &b[8];
  put_be32(p, rb.ssrc);
  p[4] = rb.fraction_lost;
  put_be24(p + 5, uint32_t(rb.cumulative_lost) & 0xFFFFFF);
  put_be32(p + 8, rb.highest_seq); put_be32(p + 12, rb.jitter);
  put_be32(p + 16, rb.lsr); put_be32(p + 20, rb.dlsr);
  return b;
}

std::vector<uint8_t> build_nack(uint32_t our, uint32_t media, const std::vector<uint16_t> &seqs) {
  std::vector<std::pair<uint16_t, uint16_t>> fci;
  for (uint16_t s : seqs) {
    if (!fci.empty()) {
      uint16_t d = uint16_t(s - fci.back().first);
      if (d >= 1 && d <= 16) { fci.back().second |= uint16_t(1 << (d - 1)); continue; }
    }
    fci.push_back({s, 0});
  }
  std::vector<uint8_t> b(12 + fci.size() * 4);
  b[0] = 0x81; b[1] = 205; put_be16(&b[2], uint16_t(2 + fci.size()));
  put_be32(&b[4], our); put_be32(&b[8], media);
  for (size_t i = 0; i < fci.size(); ++i) { put_be16(&b[12 + 4 * i], fci[i].first); put_be16(&b[14 + 4 * i], fci[i].second); }
  return b;
}

std::vector<uint8_t> build_twcc(uint32_t sender, uint32_t media, uint16_t base,
                                const std::vector<std::optional<int64_t>> &recv, uint8_t fbc) {
  int64_t first = 0;
  for (auto &r : recv) if (r) { first = *r; break; }
  int32_t ref = int32_t(first / 64000);
  std::vector<uint8_t> sym;
  std::vector<uint8_t> deltas;
  int64_t t = int64_t(ref) * 64000;
  for (auto &r : recv) {
    if (!r) { sym.push_back(0); continue; }
    int64_t d = (*r - t) / 250;
    t += d * 250;
    if (d >= 0 && d <= 255) { sym.push_back(1); deltas.push_back(uint8_t(d)); }
    else { sym.push_back(2); deltas.push_back(uint8_t(uint16_t(int16_t(d)) >> 8)); deltas.push_back(uint8_t(d)); }
  }
  std::vector<uint8_t> fci(8);
  put_be16(&fci[0], base); put_be16(&fci[2], uint16_t(recv.size()));
  put_be24(&fci[4], uint32_t(ref) & 0xFFFFFF); fci[7] = fbc;
  for (size_t i = 0; i < sym.size(); i += 7) {  // 2-bit status vector chunks
    uint16_t c = 0xC000;
    for (size_t k = 0; k < 7 && i + k < sym.size(); ++k) c |= uint16_t(sym[i + k] << (12 - 2 * k));
    fci.push_back(uint8_t(c >> 8)); fci.push_back(uint8_t(c));
  }
  fci.insert(fci.end(), deltas.begin(), deltas.end());
  while (fci.size() % 4) fci.push_back(0);
  std::vector<uint8_t> b(12);
  b[0] = 0x80 | 15; b[1] = 205; put_be16(&b[2], uint16_t((12 + fci.size()) / 4 - 1));
  put_be32(&b[4], sender); put_be32(&b[8], media);
  b.insert(b.end(), fci.begin(), fci.end());
  return b;
}

}  // namespace aoi
