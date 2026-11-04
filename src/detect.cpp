// Game auto-detection.
//
// We look at the bytes inside the file first (headers, magic numbers, logo
// checksums), because those can't lie. The file extension is only used as a
// fallback when the contents aren't conclusive.

#include "detect.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <vector>

#include "zip.hpp"

namespace fs = std::filesystem;

std::string lowerExt(const fs::path& p) {
    std::string e = p.extension().u8string();
    std::transform(e.begin(), e.end(), e.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return e;
}

namespace {

constexpr size_t kHeadSize = 0x10400;  // enough for every header we check (SNES HiROM + copier header)

uint16_t le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t le32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); }
uint32_t be32(const uint8_t* p) { return (uint32_t(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

// A view of the start of a game plus a way to fetch bytes further in.
struct Bytes {
    std::vector<uint8_t> head;
    uint64_t size = 0;
    std::function<std::vector<uint8_t>(uint64_t, size_t)> readAt;  // may be empty

    bool has(size_t off, size_t len) const { return off + len <= head.size(); }
    bool eq(size_t off, const char* s, size_t len) const {
        return has(off, len) && std::memcmp(&head[off], s, len) == 0;
    }
    const uint8_t* at(size_t off) const { return &head[off]; }
};

Detection hit(System s, const char* why) {
    Detection d;
    d.system = s;
    d.fromHeader = true;
    d.detail = why;
    return d;
}

// --- Game Boy family -------------------------------------------------------
const uint8_t kGbLogo[8] = {0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B};
const uint8_t kGbaLogo[8] = {0x24, 0xFF, 0xAE, 0x51, 0x69, 0x9A, 0xA2, 0x21};

bool gbHeaderChecksumOk(const Bytes& b) {
    if (!b.has(0x14D, 1)) return false;
    uint8_t x = 0;
    for (size_t i = 0x134; i <= 0x14C; ++i) x = uint8_t(x - b.head[i] - 1);
    return x == b.head[0x14D];
}

bool gbaComplementOk(const Bytes& b) {
    if (!b.has(0xBD, 1)) return false;
    uint8_t c = 0;
    for (size_t i = 0xA0; i <= 0xBC; ++i) c = uint8_t(c - b.head[i]);
    c = uint8_t(c - 0x19);
    return c == b.head[0xBD];
}

// --- SNES --------------------------------------------------------------------
// SNES carts have no magic number, so we score the internal header at the
// LoROM / HiROM / ExHiROM locations and accept it if it looks convincing.
int snesScore(const uint8_t* h, int kind /*0 lo, 1 hi, 2 exhi*/) {
    int score = 0;
    const uint16_t complement = le16(h + 0x1C), checksum = le16(h + 0x1E);
    if (uint16_t(complement ^ checksum) == 0xFFFF) score += 8;

    const uint8_t map = h[0x15];
    static const uint8_t lo[] = {0x20, 0x22, 0x23, 0x30, 0x32, 0x33};
    static const uint8_t hi[] = {0x21, 0x31, 0x2A, 0x3A};
    static const uint8_t ex[] = {0x25, 0x35};
    auto in = [&](const uint8_t* arr, size_t n) { return std::find(arr, arr + n, map) != arr + n; };
    if ((kind == 0 && in(lo, sizeof lo)) || (kind == 1 && in(hi, sizeof hi)) || (kind == 2 && in(ex, sizeof ex)))
        score += 4;

    const uint16_t reset = le16(h + 0x3C);
    score += reset >= 0x8000 ? 2 : -4;
    if (h[0x17] >= 0x07 && h[0x17] <= 0x0D) score += 1;  // ROM size byte is plausible
    if (h[0x16] <= 0x1A || (h[0x16] >= 0x30 && h[0x16] <= 0xF9)) score += 1;  // chipset byte

    int printable = 0;
    for (int i = 0; i < 21; ++i) printable += (h[i] >= 0x20 && h[i] < 0x7F) ? 1 : 0;
    if (printable == 21) score += 2;
    return score;
}

bool looksLikeSnes(const Bytes& b) {
    if (b.size < 0x8000 || b.size > (12u << 20) + 512) return false;
    const size_t copier = (b.size % 1024 == 512) ? 512 : 0;
    int best = -100;
    if (b.has(copier + 0x7FC0, 0x40)) best = std::max(best, snesScore(b.at(copier + 0x7FC0), 0));
    if (b.has(copier + 0xFFC0, 0x40)) best = std::max(best, snesScore(b.at(copier + 0xFFC0), 1));
    if (best < 12 && b.size > 0x410000 && b.readAt) {
        auto ex = b.readAt(copier + 0x40FFC0, 0x40);
        if (ex.size() == 0x40) best = std::max(best, snesScore(ex.data(), 2));
    }
    return best >= 12;
}

// --- GameCube / Wii disc header (raw disc layout) -----------------------------
System discSystem(const uint8_t* hdr) {
    if (be32(hdr + 0x18) == 0x5D1C9EA3) return System::Wii;
    if (be32(hdr + 0x1C) == 0xC2339F3D) return System::GameCube;
    return System::Unknown;
}

Detection detectBytes(const Bytes& b, const std::string& ext) {
    // Cartridge / disc formats with clear magic numbers first.
    if (b.eq(0, "NES\x1A", 4)) return hit(System::NES, "iNES header");
    if (b.eq(0, "UNIF", 4)) return hit(System::NES, "UNIF header");
    if (b.eq(0, "FDS\x1A", 4)) return hit(System::FDS, "fwNES disk header");
    if (b.has(0, 15) && b.head[0] == 0x01 && b.eq(1, "*NINTENDO-HVC*", 14)) return hit(System::FDS, "Famicom disk image");

    if (b.has(0, 4)) {
        const uint32_t m = be32(b.at(0));
        if (m == 0x80371240) return hit(System::N64, "N64 ROM (big-endian .z64)");
        if (m == 0x37804012) return hit(System::N64, "N64 ROM (byte-swapped .v64)");
        if (m == 0x40123780) return hit(System::N64, "N64 ROM (little-endian .n64)");
    }

    if (b.eq(0, "3DSX", 4)) return hit(System::N3DS, "3DS homebrew (3DSX)");
    if (b.eq(0x100, "NCSD", 4)) return hit(System::N3DS, "3DS cartridge image (NCSD)");
    if (b.eq(0x100, "NCCH", 4)) return hit(System::N3DS, "3DS executable (NCCH)");
    if (ext == ".cia" && b.has(0, 8) && le32(b.at(0)) == 0x2020) return hit(System::N3DS, "3DS installable (CIA)");

    // GameCube / Wii containers.
    if (b.has(0, 0x20)) {
        System s = discSystem(b.at(0));
        if (s == System::Wii) return hit(s, "Wii disc image");
        if (s == System::GameCube) return hit(s, "GameCube disc image");
    }
    if ((b.eq(0, "RVZ\x01", 4) || b.eq(0, "WIA\x01", 4)) && b.has(0x48, 4)) {
        const uint32_t t = be32(b.at(0x48));
        if (t == 1) return hit(System::GameCube, "GameCube disc (RVZ/WIA)");
        if (t == 2) return hit(System::Wii, "Wii disc (RVZ/WIA)");
    }
    if (b.has(0, 8) && le32(b.at(0)) == 0xB10BC001) {
        return le32(b.at(4)) == 1 ? hit(System::Wii, "Wii disc (GCZ)") : hit(System::GameCube, "GameCube disc (GCZ)");
    }
    if (b.eq(0, "WBFS", 4)) return hit(System::Wii, "Wii disc (WBFS)");
    if (b.eq(0, "CISO", 4) && b.has(0x8000, 0x20)) {
        System s = discSystem(b.at(0x8000));
        if (s != System::Unknown) return hit(s, s == System::Wii ? "Wii disc (CISO)" : "GameCube disc (CISO)");
    }
    if (b.has(0, 4) && be32(b.at(0)) == 0xAE0F38A2) return hit(System::GameCube, "GameCube disc (TGC)");
    if (b.has(0, 8) && be32(b.at(0)) == 0x20 && (b.eq(4, "Is\0\0", 4) || b.eq(4, "ib\0\0", 4)))
        return hit(System::Wii, "Wii channel (WAD)");

    // Wii U.
    if (b.eq(0, "WUX0", 4)) return hit(System::WiiU, "Wii U disc (WUX)");
    if (b.eq(0, "WUP-", 4)) return hit(System::WiiU, "Wii U disc (WUD)");
    if (b.eq(0, "WUHB", 4)) return hit(System::WiiU, "Wii U homebrew bundle (WUHB)");

    // Executables: Wii U RPX vs GameCube/Wii ELF.
    if (b.eq(0, "\x7F" "ELF", 4) && b.has(0, 0x14)) {
        if (b.head[7] == 0xCA && b.head[8] == 0xFE) return hit(System::WiiU, "Wii U executable (RPX)");
        if (b.head[5] == 2 && be32(b.at(0x10)) >> 16 == 0x0002 && (be32(b.at(0x10)) & 0xFFFF) == 0x0014)
            return hit(System::GameCube, "PowerPC executable (GameCube/Wii)");
    }

    // Nintendo DS: logo checksum is always 0xCF56, and the GBA-style logo sits at 0xC0.
    if (b.has(0x15C, 2) && le16(b.at(0x15C)) == 0xCF56) return hit(System::NDS, "Nintendo DS header");
    if (b.has(0xC0, 8) && std::memcmp(b.at(0xC0), kGbaLogo, 8) == 0 && b.size > 0x200)
        return hit(System::NDS, "Nintendo DS header (logo)");

    // Game Boy Advance.
    if (b.has(0xBD, 1) && b.head[0xB2] == 0x96 &&
        (std::memcmp(b.at(0x04), kGbaLogo, 8) == 0 || gbaComplementOk(b)))
        return hit(System::GBA, "Game Boy Advance header");

    // Game Boy / Game Boy Color.
    if (b.has(0x150, 1) && (std::memcmp(b.at(0x104), kGbLogo, 8) == 0 || (gbHeaderChecksumOk(b) && (ext == ".gb" || ext == ".gbc")))) {
        const uint8_t cgb = b.head[0x143];
        if (cgb == 0xC0) return hit(System::GBC, "Game Boy Color header (GBC only)");
        if (cgb == 0x80) return hit(System::GBC, "Game Boy Color header (GB compatible)");
        return hit(System::GB, "Game Boy header");
    }

    // SNES has no magic, so it goes last.
    if (looksLikeSnes(b)) return hit(System::SNES, "SNES internal header");

    // Nothing conclusive in the contents: fall back to the extension.
    Detection d;
    d.system = systemFromExtension(ext);
    if (d.system != System::Unknown) {
        // An .iso that isn't GameCube/Wii is something else entirely.
        if (ext == ".iso" || ext == ".elf") {
            d.system = System::Unknown;
            d.detail = "Disc image for a system Nintendall doesn't support";
        } else {
            d.detail = "Guessed from file extension " + ext;
        }
    } else {
        d.detail = "No known game header found";
    }
    return d;
}

Detection detectZip(const fs::path& path) {
    ZipFile z;
    Detection fallback;
    fallback.detail = "Zip file with no recognizable game inside";
    if (!z.open(path)) {
        fallback.detail = "Unreadable or corrupt zip file";
        return fallback;
    }

    // Look at every file in the archive; prefer an entry proven by its header.
    Detection guessed;
    for (const auto& e : z.entries()) {
        if (e.isDir() || e.name.rfind("__MACOSX", 0) == 0) continue;
        Bytes b;
        b.size = e.size;
        if (!z.read(e, b.head, kHeadSize) && b.head.empty()) continue;
        const std::string ext = lowerExt(fs::u8path(e.name));
        Detection d = detectBytes(b, ext);
        if (d.system == System::Unknown) continue;
        d.zipEntry = e.name;
        d.detail += " (inside zip: " + e.name + ")";
        if (d.fromHeader) return d;
        if (guessed.system == System::Unknown) guessed = d;
    }
    return guessed.system != System::Unknown ? guessed : fallback;
}

Detection detectFolder(const fs::path& dir) {
    Detection d;
    std::error_code ec;
    // Wii U games are often unpacked folders: code/*.rpx, content/, meta/.
    const fs::path code = dir / "code";
    if (fs::is_directory(code, ec)) {
        for (const auto& f : fs::directory_iterator(code, ec)) {
            if (lowerExt(f.path()) == ".rpx") {
                d.system = System::WiiU;
                d.fromHeader = true;
                d.detail = "Wii U game folder";
                d.launchPath = f.path();
                return d;
            }
        }
    }
    d.detail = "Folder";
    return d;
}

}  // namespace

Detection detectGame(const fs::path& path) {
    std::error_code ec;
    if (fs::is_directory(path, ec)) return detectFolder(path);

    Detection d;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        d.detail = "Can't open file";
        return d;
    }
    Bytes b;
    b.size = fs::file_size(path, ec);
    if (ec) b.size = 0;
    b.head.resize(size_t(std::min<uint64_t>(b.size, kHeadSize)));
    f.read(reinterpret_cast<char*>(b.head.data()), std::streamsize(b.head.size()));
    b.head.resize(size_t(f.gcount()));

    const std::string ext = lowerExt(path);
    if (b.eq(0, "PK\x03\x04", 4) || ext == ".zip") {
        d = detectZip(path);
    } else if (b.eq(0, "7z\xBC\xAF\x27\x1C", 6)) {
        d.detail = "7z archives aren't supported yet - please unzip or use .zip";
    } else {
        const fs::path p = path;
        b.readAt = [p](uint64_t off, size_t len) {
            std::vector<uint8_t> out(len);
            std::ifstream g(p, std::ios::binary);
            g.seekg(std::streamoff(off));
            g.read(reinterpret_cast<char*>(out.data()), std::streamsize(len));
            out.resize(size_t(g.gcount()));
            return out;
        };
        d = detectBytes(b, ext);
    }
    if (d.launchPath.empty()) d.launchPath = path;
    return d;
}
