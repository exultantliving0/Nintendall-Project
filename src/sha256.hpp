#pragma once
#include <filesystem>
#include <string>

// SHA-256 of a file as 64 lowercase hex digits ("" if it can't be read). Used to check a
// downloaded update against the fingerprint GitHub publishes for it.
std::string sha256File(const std::filesystem::path& file);
