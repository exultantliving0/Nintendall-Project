#include "config.hpp"

#include <fstream>

static std::string trim(const std::string& s) {
    const auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    const auto e = s.find_last_not_of(" \t\r\n");
    std::string r = s.substr(b, e - b + 1);
    if (r.size() >= 2 && r.front() == '"' && r.back() == '"') r = r.substr(1, r.size() - 2);
    return r;
}

bool Config::load(const std::filesystem::path& path) {
    path_ = path;
    values_.clear();
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        const std::string t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        const auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        values_[trim(t.substr(0, eq))] = trim(t.substr(eq + 1));
    }
    return true;
}

bool Config::save() const {
    if (path_.empty()) return false;
    std::ofstream f(path_);
    if (!f) return false;
    for (const auto& [k, v] : values_) f << k << " = \"" << v << "\"\n";
    return bool(f);
}

std::string Config::get(const std::string& key, const std::string& def) const {
    auto it = values_.find(key);
    return it == values_.end() ? def : it->second;
}

void Config::set(const std::string& key, const std::string& value) { values_[key] = value; }
