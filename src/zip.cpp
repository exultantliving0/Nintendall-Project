#include "zip.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstring>

static uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t* p) { return uint32_t(p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24)); }
static uint64_t rd64(const uint8_t* p) { return uint64_t(rd32(p)) | (uint64_t(rd32(p + 4)) << 32); }

bool ZipFile::open(const std::filesystem::path& path) {
    entries_.clear();
    f_.open(path, std::ios::binary);
    if (!f_) return false;
    f_.seekg(0, std::ios::end);
    const uint64_t fileSize = uint64_t(f_.tellg());
    if (fileSize < 22) return false;

    // The End Of Central Directory record lives in the last 64 KiB + 22 bytes.
    const uint64_t tailSize = std::min<uint64_t>(fileSize, 65535 + 22);
    std::vector<uint8_t> tail(tailSize);
    f_.seekg(std::streamoff(fileSize - tailSize));
    f_.read(reinterpret_cast<char*>(tail.data()), std::streamsize(tailSize));
    if (!f_) return false;

    int64_t eocd = -1;
    for (int64_t i = int64_t(tailSize) - 22; i >= 0; --i) {
        if (rd32(&tail[size_t(i)]) == 0x06054b50) { eocd = i; break; }
    }
    if (eocd < 0) return false;
    const uint8_t* e = &tail[size_t(eocd)];
    uint64_t count = rd16(e + 10);
    uint64_t cdSize = rd32(e + 12);
    uint64_t cdOffset = rd32(e + 16);

    // Zip64: look for the locator just before the EOCD record.
    if ((count == 0xFFFF || cdOffset == 0xFFFFFFFF) && eocd >= 20) {
        const uint8_t* loc = e - 20;
        if (rd32(loc) == 0x07064b50) {
            uint64_t z64off = rd64(loc + 8);
            uint8_t z[56];
            f_.seekg(std::streamoff(z64off));
            f_.read(reinterpret_cast<char*>(z), sizeof z);
            if (f_ && rd32(z) == 0x06064b50) {
                count = rd64(z + 32);
                cdSize = rd64(z + 40);
                cdOffset = rd64(z + 48);
            }
        }
    }
    if (cdOffset + cdSize > fileSize || cdSize > (64u << 20)) return false;

    std::vector<uint8_t> cd(static_cast<size_t>(cdSize));
    f_.seekg(std::streamoff(cdOffset));
    f_.read(reinterpret_cast<char*>(cd.data()), std::streamsize(cdSize));
    if (!f_) return false;

    size_t p = 0;
    for (uint64_t i = 0; i < count && p + 46 <= cd.size(); ++i) {
        const uint8_t* h = &cd[p];
        if (rd32(h) != 0x02014b50) break;
        ZipEntry z;
        z.method = rd16(h + 10);
        z.compSize = rd32(h + 20);
        z.size = rd32(h + 24);
        const uint16_t nameLen = rd16(h + 28), extraLen = rd16(h + 30), commentLen = rd16(h + 32);
        z.localOffset = rd32(h + 42);
        if (p + 46 + nameLen + extraLen > cd.size()) break;
        z.name.assign(reinterpret_cast<const char*>(h + 46), nameLen);

        // Zip64 extra field carries the real sizes/offset when they overflow 32 bits.
        const uint8_t* x = h + 46 + nameLen;
        size_t xp = 0;
        while (xp + 4 <= extraLen) {
            const uint16_t id = rd16(x + xp), len = rd16(x + xp + 2);
            if (id == 0x0001) {
                const uint8_t* d = x + xp + 4;
                size_t dp = 0;
                if (z.size == 0xFFFFFFFF && dp + 8 <= len) { z.size = rd64(d + dp); dp += 8; }
                if (z.compSize == 0xFFFFFFFF && dp + 8 <= len) { z.compSize = rd64(d + dp); dp += 8; }
                if (z.localOffset == 0xFFFFFFFF && dp + 8 <= len) { z.localOffset = rd64(d + dp); dp += 8; }
            }
            xp += 4 + len;
        }
        entries_.push_back(std::move(z));
        p += 46 + nameLen + extraLen + commentLen;
    }
    return true;
}

bool ZipFile::read(const ZipEntry& e, std::vector<uint8_t>& out, size_t maxBytes) {
    out.clear();
    uint8_t lh[30];
    f_.clear();
    f_.seekg(std::streamoff(e.localOffset));
    f_.read(reinterpret_cast<char*>(lh), sizeof lh);
    if (!f_ || rd32(lh) != 0x04034b50) return false;
    const uint64_t dataOff = e.localOffset + 30 + rd16(lh + 26) + rd16(lh + 28);
    f_.seekg(std::streamoff(dataOff));

    const size_t want = size_t(std::min<uint64_t>(e.size, maxBytes));

    if (e.method == 0) {  // stored
        out.resize(want);
        f_.read(reinterpret_cast<char*>(out.data()), std::streamsize(want));
        return bool(f_);
    }
    if (e.method != 8) return false;  // only deflate is supported

    z_stream zs{};
    if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
    out.resize(want);
    std::vector<uint8_t> inBuf(64 * 1024);
    uint64_t compLeft = e.compSize;
    // zlib counts with 32-bit ints, so feed the output buffer in 1 GiB slices.
    size_t produced = 0;
    zs.next_out = out.data();
    zs.avail_out = 0;
    int ret = Z_OK;
    while (ret != Z_STREAM_END) {
        if (zs.avail_out == 0) {
            produced = size_t(zs.next_out - out.data());
            if (produced >= want) break;
            zs.avail_out = uInt(std::min<size_t>(want - produced, size_t(1) << 30));
        }
        if (zs.avail_in == 0) {
            if (compLeft == 0) break;
            const size_t n = size_t(std::min<uint64_t>(compLeft, inBuf.size()));
            f_.read(reinterpret_cast<char*>(inBuf.data()), std::streamsize(n));
            if (!f_) break;
            compLeft -= n;
            zs.next_in = inBuf.data();
            zs.avail_in = uInt(n);
        }
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) break;
    }
    const size_t got = size_t(zs.next_out - out.data());
    inflateEnd(&zs);
    out.resize(got);
    return got == want;
}
