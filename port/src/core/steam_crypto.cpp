// Steam's cryptography on mbedTLS (core/steam_crypto.h).
#include "core/steam_crypto.h"

#include <mbedtls/aes.h>
#include <mbedtls/base64.h>
#include <mbedtls/bignum.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha1.h>

#include <cstring>
#include <mutex>

namespace regs::steamcrypto {

static_assert(sizeof(mbedtls_sha1_context) <= 128, "Sha1 context storage too small");

std::string rsaEncryptBase64(const std::string& modulusHex, const std::string& exponentHex, const std::string& text) {
  mbedtls_entropy_context entropy;
  mbedtls_ctr_drbg_context drbg;
  mbedtls_entropy_init(&entropy);
  mbedtls_ctr_drbg_init(&drbg);
  mbedtls_rsa_context rsa;
  mbedtls_rsa_init(&rsa);
  mbedtls_mpi n, e;
  mbedtls_mpi_init(&n);
  mbedtls_mpi_init(&e);
  std::string result;
  static const char pers[] = "opencim-steam-login";
  if (mbedtls_ctr_drbg_seed(&drbg, mbedtls_entropy_func, &entropy, reinterpret_cast<const unsigned char*>(pers),
                            sizeof pers - 1) == 0 &&
      mbedtls_mpi_read_string(&n, 16, modulusHex.c_str()) == 0 && mbedtls_mpi_read_string(&e, 16, exponentHex.c_str()) == 0 &&
      mbedtls_rsa_import(&rsa, &n, nullptr, nullptr, nullptr, &e) == 0 && mbedtls_rsa_complete(&rsa) == 0) {
    std::vector<uint8_t> out(mbedtls_rsa_get_len(&rsa));
    if (text.size() + 11 <= out.size() &&
        mbedtls_rsa_pkcs1_encrypt(&rsa, mbedtls_ctr_drbg_random, &drbg, text.size(),
                                  reinterpret_cast<const unsigned char*>(text.data()), out.data()) == 0)
      result = base64Encode(out);
  }
  mbedtls_mpi_free(&n);
  mbedtls_mpi_free(&e);
  mbedtls_rsa_free(&rsa);
  mbedtls_ctr_drbg_free(&drbg);
  mbedtls_entropy_free(&entropy);
  return result;
}

bool symmetricDecrypt(std::span<const uint8_t> in, std::span<const uint8_t> key, std::vector<uint8_t>& out) {
  out.clear();
  if (key.size() != 32 || in.size() < 32 || (in.size() - 16) % 16 != 0) return false;
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  bool ok = mbedtls_aes_setkey_dec(&aes, key.data(), 256) == 0;
  unsigned char iv[16];
  if (ok) ok = mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, in.data(), iv) == 0;
  if (ok) {
    out.resize(in.size() - 16);
    ok = mbedtls_aes_crypt_cbc(&aes, MBEDTLS_AES_DECRYPT, out.size(), iv, in.data() + 16, out.data()) == 0;
  }
  mbedtls_aes_free(&aes);
  if (!ok || out.empty()) return false;
  const uint8_t pad = out.back();
  if (pad == 0 || pad > 16 || pad > out.size()) return false;
  for (size_t i = out.size() - pad; i < out.size(); ++i)
    if (out[i] != pad) return false;
  out.resize(out.size() - pad);
  return true;
}

std::array<uint8_t, 20> sha1(std::span<const uint8_t> data) {
  std::array<uint8_t, 20> h{};
  mbedtls_sha1(data.data(), data.size(), h.data());
  return h;
}

Sha1::Sha1() {
  auto* c = reinterpret_cast<mbedtls_sha1_context*>(ctx_);
  mbedtls_sha1_init(c);
  mbedtls_sha1_starts(c);
}
Sha1::~Sha1() { mbedtls_sha1_free(reinterpret_cast<mbedtls_sha1_context*>(ctx_)); }
void Sha1::update(std::span<const uint8_t> data) {
  mbedtls_sha1_update(reinterpret_cast<mbedtls_sha1_context*>(ctx_), data.data(), data.size());
}
std::array<uint8_t, 20> Sha1::finish() {
  std::array<uint8_t, 20> h{};
  mbedtls_sha1_finish(reinterpret_cast<mbedtls_sha1_context*>(ctx_), h.data());
  return h;
}

std::vector<uint8_t> base64Decode(const std::string& s) {
  size_t n = 0;
  mbedtls_base64_decode(nullptr, 0, &n, reinterpret_cast<const unsigned char*>(s.data()), s.size());
  std::vector<uint8_t> out(n);
  if (mbedtls_base64_decode(out.data(), out.size(), &n, reinterpret_cast<const unsigned char*>(s.data()), s.size()) != 0)
    return {};
  out.resize(n);
  return out;
}

std::string base64Encode(std::span<const uint8_t> data) {
  size_t n = 0;
  mbedtls_base64_encode(nullptr, 0, &n, data.data(), data.size());
  std::string out(n, '\0');
  if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(out.data()), out.size(), &n, data.data(), data.size()) != 0)
    return {};
  out.resize(n);
  return out;
}

std::string hex(std::span<const uint8_t> data) {
  static const char* d = "0123456789abcdef";
  std::string s;
  s.reserve(data.size() * 2);
  for (uint8_t b : data) s += d[b >> 4], s += d[b & 15];
  return s;
}

}  // namespace regs::steamcrypto
