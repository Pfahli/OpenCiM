#pragma once
// The cryptography of Steam's protocol (core/steam_client.cpp), on mbedTLS: the password's RSA encryption for the
// login, the depot key's AES for file names and chunks, SHA-1 for the files' checksums, Base64.
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace regs::steamcrypto {

// RSA PKCS#1 v1.5 with the public key Steam sends (hex modulus and exponent), Base64 of the result; empty on failure.
std::string rsaEncryptBase64(const std::string& modulusHex, const std::string& exponentHex, const std::string& text);

// Steam's symmetric decryption: the first 16 bytes AES-256-ECB-decrypted are the IV of the rest, AES-256-CBC with
// PKCS#7 padding. False when the key is not 32 bytes or the data or padding is wrong.
bool symmetricDecrypt(std::span<const uint8_t> in, std::span<const uint8_t> key, std::vector<uint8_t>& out);

std::array<uint8_t, 20> sha1(std::span<const uint8_t> data);

// incremental SHA-1 (whole files)
class Sha1 {
 public:
  Sha1();
  ~Sha1();
  void update(std::span<const uint8_t> data);
  std::array<uint8_t, 20> finish();

 private:
  alignas(8) unsigned char ctx_[128];
};

std::vector<uint8_t> base64Decode(const std::string& s);
std::string base64Encode(std::span<const uint8_t> data);
std::string hex(std::span<const uint8_t> data);

}  // namespace regs::steamcrypto
