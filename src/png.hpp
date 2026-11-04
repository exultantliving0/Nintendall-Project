#pragma once
#include <cstdint>
#include <filesystem>

// Write a 24-bit RGB PNG. `rgb` is width*height*3 bytes, rows top to bottom.
bool writePng(const std::filesystem::path& path, int width, int height, const uint8_t* rgb);
