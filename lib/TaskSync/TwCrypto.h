#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

// Crypto for the TaskChampion sync protocol (v1 envelope):
// key = PBKDF2-HMAC-SHA256(secret, salt = client_id bytes, 600000 iterations),
// payload = 0x01 || nonce[12] || ChaCha20-Poly1305(ciphertext || tag[16]),
// AAD = 0x01 (app id) || version_id bytes.
// Self-contained: the C3 mbedtls build has no ChaCha20, and this keeps the
// code host-testable.
namespace twcrypto {

constexpr size_t KEY_LEN = 32;
constexpr size_t NONCE_LEN = 12;
constexpr size_t TAG_LEN = 16;
constexpr size_t AAD_LEN = 17;
constexpr size_t ENVELOPE_HEADER = 1 + NONCE_LEN;
constexpr uint32_t PBKDF2_ITERATIONS = 600000;

struct Sha256 {
  uint32_t state[8];
  uint64_t bitLen;
  uint8_t buf[64];
  size_t bufLen;
  void init();
  void update(const uint8_t* data, size_t len);
  void final(uint8_t out[32]);
};

void sha256(const uint8_t* data, size_t len, uint8_t out[32]);

// progress(done, total) is called every few thousand iterations; return false to abort.
using Pbkdf2Progress = std::function<bool(uint32_t done, uint32_t total)>;
bool pbkdf2Sha256(const uint8_t* password, size_t passwordLen, const uint8_t* salt, size_t saltLen, uint32_t iterations,
                  uint8_t* out, size_t outLen, const Pbkdf2Progress& progress = nullptr);

// Raw ChaCha20 (RFC 8439) keystream XOR, starting at block `counter`.
void chacha20Xor(const uint8_t key[32], const uint8_t nonce[12], uint32_t counter, const uint8_t* in, uint8_t* out,
                 size_t len);

struct Poly1305 {
  uint32_t r[5], h[5], pad[4];
  uint8_t buf[16];
  size_t bufLen;
  void init(const uint8_t key[32]);
  void update(const uint8_t* data, size_t len);
  void final(uint8_t tag[16]);
};

// Streaming AEAD: feed ciphertext in order (any chunk sizes), then compare tags.
class AeadStream {
 public:
  void begin(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad, size_t aadLen);
  // Decrypt mode: authenticates ciphertext and writes plaintext to out (may alias in).
  void decrypt(const uint8_t* in, uint8_t* out, size_t len);
  // Encrypt mode: writes ciphertext to out and authenticates it.
  void encrypt(const uint8_t* in, uint8_t* out, size_t len);
  // Authenticate only (no keystream). Use for a verify-first pass.
  void authOnly(const uint8_t* cipher, size_t len);
  void finish(uint8_t tag[16]);

 private:
  void crypt(const uint8_t* in, uint8_t* out, size_t len);
  uint8_t key[32];
  uint8_t nonce[12];
  uint32_t counter = 1;
  uint8_t keystream[64];
  size_t ksPos = 64;
  Poly1305 mac;
  uint64_t aadLen = 0;
  uint64_t ctLen = 0;
};

void makeAad(const uint8_t versionId[16], uint8_t aad[AAD_LEN]);

// One-shot open of a whole envelope in memory. Plaintext is written to out
// (capacity >= envelopeLen - ENVELOPE_HEADER - TAG_LEN). Returns plaintext length, or -1.
long openEnvelope(const uint8_t key[32], const uint8_t versionId[16], const uint8_t* envelope, size_t envelopeLen,
                  uint8_t* out);

// One-shot seal. out capacity >= len + ENVELOPE_HEADER + TAG_LEN. nonce must be random.
size_t sealEnvelope(const uint8_t key[32], const uint8_t versionId[16], const uint8_t nonce[12], const uint8_t* plain,
                    size_t len, uint8_t* out);

bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t len);

}  // namespace twcrypto
