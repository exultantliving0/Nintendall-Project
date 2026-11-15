#include "aes.hpp"

#include <cstring>

namespace {

// The S-box and its inverse, made once from the AES field (GF(2^8), x^8 + x^4 + x^3 + x + 1).
struct Tables {
    uint8_t sbox[256], inv[256];
    Tables() {
        uint8_t p = 1, q = 1;
        do {
            p = uint8_t(p ^ (p << 1) ^ (p & 0x80 ? 0x1B : 0));  // p * 3
            q ^= uint8_t(q << 1);                               // q / 3
            q ^= uint8_t(q << 2);
            q ^= uint8_t(q << 4);
            if (q & 0x80) q ^= 0x09;
            const uint8_t x = uint8_t(q ^ rotl(q, 1) ^ rotl(q, 2) ^ rotl(q, 3) ^ rotl(q, 4) ^ 0x63);
            sbox[p] = x;
            inv[x] = p;
        } while (p != 1);
        sbox[0] = 0x63;
        inv[0x63] = 0;
    }
    static uint8_t rotl(uint8_t v, int n) { return uint8_t(v << n | v >> (8 - n)); }
};
const Tables& tables() {
    static const Tables t;
    return t;
}

uint8_t xtime(uint8_t v) { return uint8_t(v << 1 ^ (v & 0x80 ? 0x1B : 0)); }
uint8_t mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

// The 11 round keys (176 bytes).
void expandKey(const uint8_t key[16], uint8_t out[176]) {
    const Tables& t = tables();
    std::memcpy(out, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t w[4] = {out[i - 4], out[i - 3], out[i - 2], out[i - 1]};
        if (i % 16 == 0) {
            const uint8_t first = w[0];
            w[0] = uint8_t(t.sbox[w[1]] ^ rcon);
            w[1] = t.sbox[w[2]];
            w[2] = t.sbox[w[3]];
            w[3] = t.sbox[first];
            rcon = xtime(rcon);
        }
        for (int j = 0; j < 4; ++j) out[i + j] = uint8_t(out[i - 16 + j] ^ w[j]);
    }
}

void decryptBlock(const uint8_t rk[176], uint8_t s[16]) {
    const Tables& t = tables();
    for (int i = 0; i < 16; ++i) s[i] ^= rk[160 + i];
    for (int round = 9; round >= 0; --round) {
        // Inverse ShiftRows (the state is column by column: s[row + 4 * column]).
        uint8_t tmp[16];
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r) tmp[r + 4 * ((c + r) % 4)] = s[r + 4 * c];
        // Inverse SubBytes, AddRoundKey.
        for (int i = 0; i < 16; ++i) s[i] = uint8_t(t.inv[tmp[i]] ^ rk[16 * round + i]);
        if (round == 0) break;
        // Inverse MixColumns.
        for (int c = 0; c < 4; ++c) {
            uint8_t* col = s + 4 * c;
            const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
            col[0] = uint8_t(mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9));
            col[1] = uint8_t(mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13));
            col[2] = uint8_t(mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11));
            col[3] = uint8_t(mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14));
        }
    }
}

}  // namespace

void aes128CbcDecrypt(const uint8_t key[16], uint8_t iv[16], uint8_t* data, size_t size) {
    uint8_t rk[176];
    expandKey(key, rk);
    for (size_t off = 0; off + 16 <= size; off += 16) {
        uint8_t* block = data + off;
        uint8_t next[16];
        std::memcpy(next, block, 16);
        decryptBlock(rk, block);
        for (int i = 0; i < 16; ++i) block[i] ^= iv[i];
        std::memcpy(iv, next, 16);
    }
}
