#include "update.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <vector>

#include "http.hpp"

namespace {

// Where Nintendall's releases are published.
const char* kRepo = "exultantliving0/Nintendall-Project";

// ---------------------------------------------------------------------------
// A small JSON reader: enough for GitHub's list of releases.

struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    std::string s;  // strings (and numbers, as text)
    std::vector<Json> items;
    std::map<std::string, Json> fields;

    const Json& operator[](const std::string& key) const {
        static const Json none;
        const auto it = fields.find(key);
        return it == fields.end() ? none : it->second;
    }
};

class Parser {
public:
    explicit Parser(const std::string& text) : t_(text) {}
    bool parse(Json& out) {
        if (!value(out, 0)) return false;
        space();
        return p_ == t_.size();
    }

private:
    void space() {
        while (p_ < t_.size() && std::isspace(static_cast<unsigned char>(t_[p_]))) ++p_;
    }
    bool literal(const char* word) {
        const std::string w(word);
        if (t_.compare(p_, w.size(), w) != 0) return false;
        p_ += w.size();
        return true;
    }
    static void utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += char(cp);
        } else if (cp < 0x800) {
            out += char(0xC0 | cp >> 6);
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | cp >> 12);
            out += char(0x80 | (cp >> 6 & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | cp >> 18);
            out += char(0x80 | (cp >> 12 & 0x3F));
            out += char(0x80 | (cp >> 6 & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(unsigned& v) {
        if (p_ + 4 > t_.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = t_[p_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else return false;
        }
        return true;
    }
    bool string(std::string& out) {
        if (t_[p_] != '"') return false;
        ++p_;
        while (p_ < t_.size()) {
            const char c = t_[p_++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (p_ >= t_.size()) return false;
            const char e = t_[p_++];
            switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                unsigned cp;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && t_.compare(p_, 2, "\\u") == 0) {  // surrogate pair
                    p_ += 2;
                    unsigned lo;
                    if (!hex4(lo)) return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8(out, cp);
                break;
            }
            default: out += e; break;  // \" \\ \/
            }
        }
        return false;
    }
    bool value(Json& v, int depth) {
        if (depth > 64) return false;
        space();
        if (p_ >= t_.size()) return false;
        const char c = t_[p_];
        if (c == '{') {
            v.type = Json::Object;
            ++p_;
            space();
            if (p_ < t_.size() && t_[p_] == '}') return ++p_, true;
            for (;;) {
                space();
                std::string key;
                if (p_ >= t_.size() || !string(key)) return false;
                space();
                if (p_ >= t_.size() || t_[p_++] != ':') return false;
                if (!value(v.fields[key], depth + 1)) return false;
                space();
                if (p_ >= t_.size()) return false;
                if (t_[p_] == ',') ++p_;
                else if (t_[p_] == '}') return ++p_, true;
                else return false;
            }
        }
        if (c == '[') {
            v.type = Json::Array;
            ++p_;
            space();
            if (p_ < t_.size() && t_[p_] == ']') return ++p_, true;
            for (;;) {
                v.items.emplace_back();
                if (!value(v.items.back(), depth + 1)) return false;
                space();
                if (p_ >= t_.size()) return false;
                if (t_[p_] == ',') ++p_;
                else if (t_[p_] == ']') return ++p_, true;
                else return false;
            }
        }
        if (c == '"') {
            v.type = Json::String;
            return string(v.s);
        }
        if (literal("true")) return v.type = Json::Bool, v.b = true, true;
        if (literal("false")) return v.type = Json::Bool, true;
        if (literal("null")) return true;
        v.type = Json::Number;
        while (p_ < t_.size() && (std::isdigit(static_cast<unsigned char>(t_[p_])) || std::strchr("+-.eE", t_[p_])))
            v.s += t_[p_++];
        return !v.s.empty();
    }

    const std::string& t_;
    size_t p_ = 0;
};

std::vector<int> versionParts(const std::string& v) {
    std::vector<int> parts;
    size_t i = (!v.empty() && (v[0] == 'v' || v[0] == 'V')) ? 1 : 0;
    while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) {
        int n = 0;
        while (i < v.size() && std::isdigit(static_cast<unsigned char>(v[i]))) n = n * 10 + (v[i++] - '0');
        parts.push_back(n);
        if (i < v.size() && v[i] == '.') ++i;
        else break;
    }
    return parts;
}

// Splits a line into lines of at most `width` characters (at spaces); later lines get `indent`.
std::vector<std::string> wrapLine(const std::string& text, size_t width, const std::string& indent) {
    std::vector<std::string> out;
    const size_t lead = text.find_first_not_of(' ');
    std::string line = text.substr(0, lead == std::string::npos ? 0 : lead);  // keep the first line's indent
    bool empty = true;
    std::istringstream words(text);
    for (std::string word; words >> word;) {
        if (!empty && line.size() + 1 + word.size() > width) {
            out.push_back(line);
            line = indent;
            empty = true;
        }
        line += (empty ? "" : " ") + word;
        empty = false;
    }
    if (!empty) out.push_back(line);
    return out;
}

// The first few lines of the release notes as plain text (Markdown marks taken out), wrapped
// to fit a message box.
std::string shortNotes(const std::string& body) {
    std::istringstream in(body);
    std::string line, out;
    int lines = 0;
    while (std::getline(in, line) && lines < 10) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.find("Generated with") != std::string::npos) continue;
        size_t a = 0;
        while (a < line.size() && (line[a] == '#' || line[a] == ' ')) ++a;
        std::string t = line.substr(a);
        for (size_t p; (p = t.find("**")) != std::string::npos;) t.erase(p, 2);
        for (size_t p; (p = t.find('`')) != std::string::npos;) t.erase(p, 1);
        if (t.rfind("- ", 0) == 0 || t.rfind("* ", 0) == 0) t = "  \xE2\x80\xA2 " + t.substr(2);  // bullet
        if (t.empty()) {  // paragraph break
            if (!out.empty() && out.compare(out.size() - std::min<size_t>(2, out.size()), 2, "\n\n") != 0) out += "\n";
            continue;
        }
        const bool bullet = t.rfind("  \xE2\x80\xA2 ", 0) == 0;
        for (const std::string& part : wrapLine(t, 72, bullet ? "    " : "")) {
            if (lines >= 10) break;
            out += part + "\n";
            ++lines;
        }
    }
    while (!out.empty() && out.back() == '\n') out.pop_back();
    return out;
}

}  // namespace

bool isNewerVersion(const std::string& a, const std::string& b) {
    std::vector<int> x = versionParts(a), y = versionParts(b);
    if (x.empty() || y.empty()) return false;
    x.resize(std::max(x.size(), y.size()), 0);
    y.resize(x.size(), 0);
    return x > y;
}

UpdateInfo checkForUpdate(const std::string& currentVersion) {
    UpdateInfo info;
    std::vector<uint8_t> body;
    if (httpGet(std::string("https://api.github.com/repos/") + kRepo + "/releases?per_page=20", body) != 200) return info;
    const std::string text(body.begin(), body.end());
    Json list;
    if (!Parser(text).parse(list) || list.type != Json::Array) return info;

    // The newest published release (pre-releases count: that's how Nintendall is released).
    const Json* best = nullptr;
    for (const Json& r : list.items) {
        if (r["draft"].b || r["tag_name"].type != Json::String) continue;
        if (!best || isNewerVersion(r["tag_name"].s, (*best)["tag_name"].s)) best = &r;
    }
    if (!best || !isNewerVersion((*best)["tag_name"].s, currentVersion)) return info;

    const std::string tag = (*best)["tag_name"].s;
    info.available = true;
    info.version = tag[0] == 'v' || tag[0] == 'V' ? tag.substr(1) : tag;
    info.pageUrl = (*best)["html_url"].type == Json::String && (*best)["html_url"].s.rfind("https://github.com/", 0) == 0
                       ? (*best)["html_url"].s
                       : std::string("https://github.com/") + kRepo + "/releases/tag/" + urlEncode(tag);
    info.notes = shortNotes((*best)["body"].s);
    return info;
}
