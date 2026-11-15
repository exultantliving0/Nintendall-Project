#pragma once
#include <cstddef>
#include <cstdint>

// AES-128 decryption in CBC mode (for the Wii's save files: see saves.cpp). `data` is decrypted in
// place; its size must be a multiple of 16. `iv` is updated, so a long file can be done in parts.
void aes128CbcDecrypt(const uint8_t key[16], uint8_t iv[16], uint8_t* data, size_t size);
