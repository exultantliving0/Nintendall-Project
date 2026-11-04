#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// Minimal read-only .zip reader (stored + deflate), enough to look inside
// zipped games and pull out the ROM.
struct ZipEntry {
    std::string name;
    uint16_t method = 0;
    uint64_t compSize = 0;
    uint64_t size = 0;
    uint64_t localOffset = 0;
    bool isDir() const { return !name.empty() && name.back() == '/'; }
};

class ZipFile {
public:
    bool open(const std::filesystem::path& path);
    const std::vector<ZipEntry>& entries() const { return entries_; }
    // Decompress an entry. Stops after maxBytes (useful for reading just a header).
    bool read(const ZipEntry& e, std::vector<uint8_t>& out, size_t maxBytes = SIZE_MAX);

private:
    std::ifstream f_;
    std::vector<ZipEntry> entries_;
};
