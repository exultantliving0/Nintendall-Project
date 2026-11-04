#pragma once
#include <filesystem>
#include <map>
#include <string>

// Tiny "key = value" settings file.
class Config {
public:
    bool load(const std::filesystem::path& path);
    bool save() const;
    std::string get(const std::string& key, const std::string& def = "") const;
    void set(const std::string& key, const std::string& value);
    const std::map<std::string, std::string>& all() const { return values_; }

private:
    std::filesystem::path path_;
    std::map<std::string, std::string> values_;
};
