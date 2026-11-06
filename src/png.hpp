#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

// Write a 24-bit RGB PNG. `rgb` is width*height*3 bytes, rows top to bottom.
bool writePng(const std::filesystem::path& path, int width, int height, const uint8_t* rgb);
// The same PNG, in memory (empty if it failed).
std::vector<uint8_t> encodePng(int width, int height, const uint8_t* rgb);
