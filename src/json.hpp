#pragma once
#include <cctype>
#include <cstring>
#include <map>
#include <string>
#include <vector>

// A small JSON reader: enough for GitHub's list of releases and the Internet Archive's answers.
// JsonParser(text).parse(json) is false if the text isn't valid JSON.

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

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : t_(text) {}
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
