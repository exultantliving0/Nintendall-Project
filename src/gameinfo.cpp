#include "gameinfo.hpp"

#include <fstream>
#include <map>
#include <unordered_map>

namespace fs = std::filesystem;

namespace {

// Accented letters -> plain ones (the same table as tools/make_game_info.py).
char foldLetter(unsigned cp) {
    static const std::map<unsigned, char> table = [] {
        std::map<unsigned, char> t;
        const std::pair<char, const char*> groups[] = {
            {'a', "àáâãäåāÀÁÂÃÄÅĀ"}, {'c', "çćÇĆ"}, {'e', "èéêëēÈÉÊËĒ"}, {'i', "ìíîïīÌÍÎÏĪ"},
            {'n', "ñÑ"}, {'o', "òóôõöøōÒÓÔÕÖØŌ"}, {'u', "ùúûüūÙÚÛÜŪ"}, {'y', "ýÿÝŸ"},
        };
        for (const auto& [plain, letters] : groups) {
            const std::string s = letters;
            for (size_t i = 0; i < s.size();) {
                const unsigned char c = static_cast<unsigned char>(s[i]);
                unsigned cp = c;
                size_t n = 1;
                if ((c & 0xE0) == 0xC0) cp = c & 0x1Fu, n = 2;
                else if ((c & 0xF0) == 0xE0) cp = c & 0x0Fu, n = 3;
                for (size_t k = 1; k < n && i + k < s.size(); ++k) cp = cp << 6 | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
                t[cp] = plain;
                i += n;
            }
        }
        return t;
    }();
    const auto it = table.find(cp);
    return it == table.end() ? 0 : it->second;
}

struct ConsoleInfo {
    std::unordered_map<std::string, std::string> lines;  // name key -> the rest of its line
};

std::string unescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            ++i;
            out += s[i] == 'n' ? '\n' : s[i];
        } else {
            out += s[i];
        }
    }
    return out;
}

const ConsoleInfo& consoleInfo(const fs::path& dir, const std::string& key) {
    static std::map<std::string, ConsoleInfo> loaded;
    const auto it = loaded.find(key);
    if (it != loaded.end()) return it->second;
    ConsoleInfo& info = loaded[key];
    std::ifstream f(dir / (key + ".txt"), std::ios::binary);
    for (std::string line; std::getline(f, line);) {
        const size_t tab = line.find('\t');
        if (tab == std::string::npos || tab == 0) continue;
        info.lines.emplace(line.substr(0, tab), line.substr(tab + 1));
    }
    return info;
}

}  // namespace

std::vector<std::string> nameWords(const std::string& name) {
    std::string text;  // folded, lower case, "&" as " and "
    for (size_t i = 0; i < name.size();) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (c < 0x80) {
            text += c == '&' ? std::string(" and ") : std::string(1, char(c >= 'A' && c <= 'Z' ? c + 32 : c));
            ++i;
            continue;
        }
        unsigned cp = 0;
        size_t n = 1;
        if ((c & 0xE0) == 0xC0) cp = c & 0x1Fu, n = 2;
        else if ((c & 0xF0) == 0xE0) cp = c & 0x0Fu, n = 3;
        else if ((c & 0xF8) == 0xF0) cp = c & 0x07u, n = 4;
        for (size_t k = 1; k < n && i + k < name.size(); ++k) cp = cp << 6 | (static_cast<unsigned char>(name[i + k]) & 0x3Fu);
        const char plain = foldLetter(cp);
        text += plain ? plain : ' ';
        i += n;
    }
    std::vector<std::string> words;
    std::string word;
    for (const char c : text + " ") {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            word += c;
        } else if (!word.empty()) {
            words.push_back(word);
            word.clear();
        }
    }
    return words;
}

std::string gameInfoKey(const std::string& name) {
    std::string key;
    for (const std::string& w : nameWords(name))
        if (w != "the") key += w;
    return key;
}

bool findGameInfo(const fs::path& dir, const LibraryGame& game, GameInfo& out) {
    std::string console = systemInfo(game.system).key;
    if (game.system == System::FDS) console = systemInfo(System::NES).key;  // FDS games are with the NES ones
    const ConsoleInfo& info = consoleInfo(dir, console);
    if (info.lines.empty()) return false;

    for (const std::string& name : {game.title, cleanTitle(game.fileName)}) {
        std::string key = gameInfoKey(name);
        auto it = info.lines.find(key);
        if (it != info.lines.end() && it->second.rfind('=', 0) == 0) it = info.lines.find(it->second.substr(1));
        if (key.empty() || it == info.lines.end()) continue;

        // title TAB released TAB players TAB back picture TAB description
        std::string parts[5];
        size_t a = 0;
        for (int i = 0; i < 5; ++i) {
            const size_t b = i == 4 ? std::string::npos : it->second.find('\t', a);
            parts[i] = it->second.substr(a, b == std::string::npos ? std::string::npos : b - a);
            if (b == std::string::npos) break;
            a = b + 1;
        }
        out.title = parts[0];
        out.released = parts[1];
        out.players = parts[2];
        out.backUrl = parts[3].empty() ? "" : "https://cdn.thegamesdb.net/images/large/" + parts[3];
        out.overview = unescape(parts[4]);
        return true;
    }
    return false;
}
