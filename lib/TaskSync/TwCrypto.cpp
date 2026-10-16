#include "TwCrypto.h"

#include <algorithm>
#include <cstring>

namespace twcrypto {

namespace {

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
inline uint32_t load32be(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline void store32be(uint8_t* p, uint32_t v) {
  p[0] = v >> 24;
  p[1] = v >> 16;
  p[2] = v >> 8;
  p[3] = v;
}
inline uint32_t load32le(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline void store32le(uint8_t* p, uint32_t v) {
  p[0] = v;
  p[1] = v >> 8;
  p[2] = v >> 16;
  p[3] = v >> 24;
}

constexpr uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

void sha256Block(uint32_t state[8], const uint8_t block[64]) {
  uint32_t w[64];
  for (int i = 0; i < 16; ++i) w[i] = load32be(block + i * 4);
  for (int i = 16; i < 64; ++i) {
    const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6],
           h = state[7];
  for (int i = 0; i < 64; ++i) {
    const uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
    const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

// HMAC-SHA256 of a 32-byte message from precomputed inner/outer pad states.
// Two compression calls per HMAC: the hot loop of PBKDF2.
void hmac32(const uint32_t inner[8], const uint32_t outer[8], const uint8_t msg[32], uint8_t out[32]) {
  uint8_t block[64];
  uint32_t st[8];
  memcpy(block, msg, 32);
  block[32] = 0x80;
  memset(block + 33, 0, 64 - 33 - 8);
  // Length = 64 (pad block) + 32 bytes = 768 bits.
  memset(block + 56, 0, 8);
  block[62] = 0x03;
  block[63] = 0x00;
  memcpy(st, inner, sizeof(st));
  sha256Block(st, block);
  for (int i = 0; i < 8; ++i) store32be(block + i * 4, st[i]);
  memcpy(st, outer, sizeof(st));
  sha256Block(st, block);
  for (int i = 0; i < 8; ++i) store32be(out + i * 4, st[i]);
}

void chachaBlock(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, uint8_t out[64]) {
  uint32_t s[16] = {0x61707865, 0x3320646e, 0x79622d32, 0x6b206574};
  for (int i = 0; i < 8; ++i) s[4 + i] = load32le(key + i * 4);
  s[12] = counter;
  for (int i = 0; i < 3; ++i) s[13 + i] = load32le(nonce + i * 4);
  uint32_t x[16];
  memcpy(x, s, sizeof(x));
#define QR(a, b, c, d) \
  a += b;              \
  d = rotl(d ^ a, 16); \
  c += d;              \
  b = rotl(b ^ c, 12); \
  a += b;              \
  d = rotl(d ^ a, 8);  \
  c += d;              \
  b = rotl(b ^ c, 7);
  for (int i = 0; i < 10; ++i) {
    QR(x[0], x[4], x[8], x[12])
    QR(x[1], x[5], x[9], x[13])
    QR(x[2], x[6], x[10], x[14])
    QR(x[3], x[7], x[11], x[15])
    QR(x[0], x[5], x[10], x[15])
    QR(x[1], x[6], x[11], x[12])
    QR(x[2], x[7], x[8], x[13])
    QR(x[3], x[4], x[9], x[14])
  }
#undef QR
  for (int i = 0; i < 16; ++i) store32le(out + i * 4, x[i] + s[i]);
}

void polyBlocks(Poly1305& p, const uint8_t* m, size_t blocks, uint32_t hibit) {
  const uint32_t r0 = p.r[0], r1 = p.r[1], r2 = p.r[2], r3 = p.r[3], r4 = p.r[4];
  const uint32_t s1 = r1 * 5, s2 = r2 * 5, s3 = r3 * 5, s4 = r4 * 5;
  uint32_t h0 = p.h[0], h1 = p.h[1], h2 = p.h[2], h3 = p.h[3], h4 = p.h[4];
  while (blocks--) {
    h0 += load32le(m) & 0x3ffffff;
    h1 += (load32le(m + 3) >> 2) & 0x3ffffff;
    h2 += (load32le(m + 6) >> 4) & 0x3ffffff;
    h3 += (load32le(m + 9) >> 6) & 0x3ffffff;
    h4 += (load32le(m + 12) >> 8) | hibit;
    const uint64_t d0 =
        uint64_t(h0) * r0 + uint64_t(h1) * s4 + uint64_t(h2) * s3 + uint64_t(h3) * s2 + uint64_t(h4) * s1;
    uint64_t d1 = uint64_t(h0) * r1 + uint64_t(h1) * r0 + uint64_t(h2) * s4 + uint64_t(h3) * s3 + uint64_t(h4) * s2;
    uint64_t d2 = uint64_t(h0) * r2 + uint64_t(h1) * r1 + uint64_t(h2) * r0 + uint64_t(h3) * s4 + uint64_t(h4) * s3;
    uint64_t d3 = uint64_t(h0) * r3 + uint64_t(h1) * r2 + uint64_t(h2) * r1 + uint64_t(h3) * r0 + uint64_t(h4) * s4;
    uint64_t d4 = uint64_t(h0) * r4 + uint64_t(h1) * r3 + uint64_t(h2) * r2 + uint64_t(h3) * r1 + uint64_t(h4) * r0;
    uint32_t c = uint32_t(d0 >> 26);
    h0 = uint32_t(d0) & 0x3ffffff;
    d1 += c;
    c = uint32_t(d1 >> 26);
    h1 = uint32_t(d1) & 0x3ffffff;
    d2 += c;
    c = uint32_t(d2 >> 26);
    h2 = uint32_t(d2) & 0x3ffffff;
    d3 += c;
    c = uint32_t(d3 >> 26);
    h3 = uint32_t(d3) & 0x3ffffff;
    d4 += c;
    c = uint32_t(d4 >> 26);
    h4 = uint32_t(d4) & 0x3ffffff;
    h0 += c * 5;
    c = h0 >> 26;
    h0 &= 0x3ffffff;
    h1 += c;
    m += 16;
  }
  p.h[0] = h0;
  p.h[1] = h1;
  p.h[2] = h2;
  p.h[3] = h3;
  p.h[4] = h4;
}

}  // namespace

void Sha256::init() {
  static constexpr uint32_t IV[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(state, IV, sizeof(state));
  bitLen = 0;
  bufLen = 0;
}

void Sha256::update(const uint8_t* data, size_t len) {
  bitLen += uint64_t(len) * 8;
  while (len > 0) {
    const size_t take = std::min(len, sizeof(buf) - bufLen);
    memcpy(buf + bufLen, data, take);
    bufLen += take;
    data += take;
    len -= take;
    if (bufLen == 64) {
      sha256Block(state, buf);
      bufLen = 0;
    }
  }
}

void Sha256::final(uint8_t out[32]) {
  const uint64_t bits = bitLen;
  buf[bufLen++] = 0x80;
  if (bufLen > 56) {
    memset(buf + bufLen, 0, 64 - bufLen);
    sha256Block(state, buf);
    bufLen = 0;
  }
  memset(buf + bufLen, 0, 56 - bufLen);
  for (int i = 0; i < 8; ++i) buf[56 + i] = uint8_t(bits >> (56 - 8 * i));
  sha256Block(state, buf);
  for (int i = 0; i < 8; ++i) store32be(out + i * 4, state[i]);
}

void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
  Sha256 s;
  s.init();
  s.update(data, len);
  s.final(out);
}

bool pbkdf2Sha256(const uint8_t* password, size_t passwordLen, const uint8_t* salt, size_t saltLen, uint32_t iterations,
                  uint8_t* out, size_t outLen, const Pbkdf2Progress& progress) {
  uint8_t keyBlock[64] = {};
  if (passwordLen > 64) {
    sha256(password, passwordLen, keyBlock);
  } else {
    memcpy(keyBlock, password, passwordLen);
  }
  uint8_t pad[64];
  Sha256 inner, outer;
  for (int i = 0; i < 64; ++i) pad[i] = keyBlock[i] ^ 0x36;
  inner.init();
  inner.update(pad, 64);
  for (int i = 0; i < 64; ++i) pad[i] = keyBlock[i] ^ 0x5c;
  outer.init();
  outer.update(pad, 64);

  const uint32_t blocks = (outLen + 31) / 32;
  const uint32_t total = blocks * iterations;
  for (uint32_t block = 1; block <= blocks; ++block) {
    // U1 = HMAC(P, S || INT(block)), full HMAC since the message is not 32 bytes.
    uint8_t be[4];
    store32be(be, block);
    Sha256 h = inner;
    h.update(salt, saltLen);
    h.update(be, 4);
    uint8_t u[32];
    h.final(u);
    h = outer;
    h.update(u, 32);
    h.final(u);

    uint8_t t[32];
    memcpy(t, u, 32);
    for (uint32_t i = 1; i < iterations; ++i) {
      hmac32(inner.state, outer.state, u, u);
      for (int j = 0; j < 32; ++j) t[j] ^= u[j];
      if (progress && (i & 0xfff) == 0 && !progress((block - 1) * iterations + i, total)) return false;
    }
    const size_t off = (block - 1) * 32;
    memcpy(out + off, t, std::min<size_t>(32, outLen - off));
  }
  return true;
}

void chacha20Xor(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, const uint8_t* in, uint8_t* out,
                 size_t len) {
  uint8_t ks[64];
  while (len > 0) {
    chachaBlock(key, nonce, counter++, ks);
    const size_t n = std::min<size_t>(64, len);
    for (size_t i = 0; i < n; ++i) out[i] = in[i] ^ ks[i];
    in += n;
    out += n;
    len -= n;
  }
}

void Poly1305::init(const uint8_t key[32]) {
  r[0] = load32le(key) & 0x3ffffff;
  r[1] = (load32le(key + 3) >> 2) & 0x3ffff03;
  r[2] = (load32le(key + 6) >> 4) & 0x3ffc0ff;
  r[3] = (load32le(key + 9) >> 6) & 0x3f03fff;
  r[4] = (load32le(key + 12) >> 8) & 0x00fffff;
  for (int i = 0; i < 5; ++i) h[i] = 0;
  for (int i = 0; i < 4; ++i) pad[i] = load32le(key + 16 + i * 4);
  bufLen = 0;
}

void Poly1305::update(const uint8_t* data, size_t len) {
  if (bufLen) {
    const size_t take = std::min(len, 16 - bufLen);
    memcpy(buf + bufLen, data, take);
    bufLen += take;
    data += take;
    len -= take;
    if (bufLen < 16) return;
    polyBlocks(*this, buf, 1, 1u << 24);
    bufLen = 0;
  }
  if (len >= 16) {
    const size_t blocks = len / 16;
    polyBlocks(*this, data, blocks, 1u << 24);
    data += blocks * 16;
    len -= blocks * 16;
  }
  memcpy(buf, data, len);
  bufLen = len;
}

void Poly1305::final(uint8_t tag[16]) {
  if (bufLen) {
    buf[bufLen] = 1;
    memset(buf + bufLen + 1, 0, 16 - bufLen - 1);
    polyBlocks(*this, buf, 1, 0);
  }
  uint32_t h0 = h[0], h1 = h[1], h2 = h[2], h3 = h[3], h4 = h[4];
  uint32_t c = h1 >> 26;
  h1 &= 0x3ffffff;
  h2 += c;
  c = h2 >> 26;
  h2 &= 0x3ffffff;
  h3 += c;
  c = h3 >> 26;
  h3 &= 0x3ffffff;
  h4 += c;
  c = h4 >> 26;
  h4 &= 0x3ffffff;
  h0 += c * 5;
  c = h0 >> 26;
  h0 &= 0x3ffffff;
  h1 += c;

  uint32_t g0 = h0 + 5;
  c = g0 >> 26;
  g0 &= 0x3ffffff;
  uint32_t g1 = h1 + c;
  c = g1 >> 26;
  g1 &= 0x3ffffff;
  uint32_t g2 = h2 + c;
  c = g2 >> 26;
  g2 &= 0x3ffffff;
  uint32_t g3 = h3 + c;
  c = g3 >> 26;
  g3 &= 0x3ffffff;
  uint32_t g4 = h4 + c - (1u << 26);

  // Select h - p when h >= p (g4 did not underflow).
  uint32_t mask = (g4 >> 31) - 1;
  g0 &= mask;
  g1 &= mask;
  g2 &= mask;
  g3 &= mask;
  g4 &= mask;
  mask = ~mask;
  h0 = (h0 & mask) | g0;
  h1 = (h1 & mask) | g1;
  h2 = (h2 & mask) | g2;
  h3 = (h3 & mask) | g3;
  h4 = (h4 & mask) | g4;

  const uint32_t w0 = h0 | (h1 << 26);
  const uint32_t w1 = (h1 >> 6) | (h2 << 20);
  const uint32_t w2 = (h2 >> 12) | (h3 << 14);
  const uint32_t w3 = (h3 >> 18) | (h4 << 8);

  uint64_t f = uint64_t(w0) + pad[0];
  store32le(tag, uint32_t(f));
  f = uint64_t(w1) + pad[1] + (f >> 32);
  store32le(tag + 4, uint32_t(f));
  f = uint64_t(w2) + pad[2] + (f >> 32);
  store32le(tag + 8, uint32_t(f));
  f = uint64_t(w3) + pad[3] + (f >> 32);
  store32le(tag + 12, uint32_t(f));
}

void AeadStream::begin(const uint8_t k[32], const uint8_t n[12], const uint8_t* aad, size_t aadLength) {
  memcpy(key, k, 32);
  memcpy(nonce, n, 12);
  uint8_t block0[64];
  chachaBlock(key, nonce, 0, block0);
  mac.init(block0);
  mac.update(aad, aadLength);
  static constexpr uint8_t ZEROS[16] = {};
  if (aadLength % 16) mac.update(ZEROS, 16 - aadLength % 16);
  aadLen = aadLength;
  ctLen = 0;
  counter = 1;
  ksPos = 64;
}

void AeadStream::crypt(const uint8_t* in, uint8_t* out, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    if (ksPos == 64) {
      chachaBlock(key, nonce, counter++, keystream);
      ksPos = 0;
    }
    out[i] = in[i] ^ keystream[ksPos++];
  }
}

void AeadStream::decrypt(const uint8_t* in, uint8_t* out, size_t len) {
  mac.update(in, len);
  ctLen += len;
  crypt(in, out, len);
}

void AeadStream::encrypt(const uint8_t* in, uint8_t* out, size_t len) {
  crypt(in, out, len);
  mac.update(out, len);
  ctLen += len;
}

void AeadStream::authOnly(const uint8_t* cipher, size_t len) {
  mac.update(cipher, len);
  ctLen += len;
}

void AeadStream::finish(uint8_t tag[16]) {
  static constexpr uint8_t ZEROS[16] = {};
  if (ctLen % 16) mac.update(ZEROS, 16 - ctLen % 16);
  uint8_t lens[16];
  for (int i = 0; i < 8; ++i) {
    lens[i] = uint8_t(aadLen >> (8 * i));
    lens[8 + i] = uint8_t(ctLen >> (8 * i));
  }
  mac.update(lens, 16);
  mac.final(tag);
}

void makeAad(const uint8_t versionId[16], uint8_t aad[AAD_LEN]) {
  aad[0] = 1;  // TaskChampion app id
  memcpy(aad + 1, versionId, 16);
}

bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t len) {
  uint8_t diff = 0;
  for (size_t i = 0; i < len; ++i) diff |= a[i] ^ b[i];
  return diff == 0;
}

long openEnvelope(const uint8_t key[32], const uint8_t versionId[16], const uint8_t* envelope, size_t envelopeLen,
                  uint8_t* out) {
  if (envelopeLen < ENVELOPE_HEADER + TAG_LEN || envelope[0] != 1) return -1;
  const size_t ctLen = envelopeLen - ENVELOPE_HEADER - TAG_LEN;
  uint8_t aad[AAD_LEN];
  makeAad(versionId, aad);
  AeadStream aead;
  aead.begin(key, envelope + 1, aad, AAD_LEN);
  aead.authOnly(envelope + ENVELOPE_HEADER, ctLen);
  uint8_t tag[TAG_LEN];
  aead.finish(tag);
  if (!constantTimeEqual(tag, envelope + ENVELOPE_HEADER + ctLen, TAG_LEN)) return -1;
  chacha20Xor(key, envelope + 1, 1, envelope + ENVELOPE_HEADER, out, ctLen);
  return static_cast<long>(ctLen);
}

size_t sealEnvelope(const uint8_t key[32], const uint8_t versionId[16], const uint8_t nonce[12], const uint8_t* plain,
                    size_t len, uint8_t* out) {
  uint8_t aad[AAD_LEN];
  makeAad(versionId, aad);
  out[0] = 1;
  memcpy(out + 1, nonce, NONCE_LEN);
  AeadStream aead;
  aead.begin(key, nonce, aad, AAD_LEN);
  aead.encrypt(plain, out + ENVELOPE_HEADER, len);
  aead.finish(out + ENVELOPE_HEADER + len);
  return ENVELOPE_HEADER + len + TAG_LEN;
}

}  // namespace twcrypto
