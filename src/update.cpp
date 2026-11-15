#include "update.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>
#include <memory>
#include <sstream>
#include <vector>

#include "http.hpp"
#include "json.hpp"

namespace {

// Where Nintendall's releases are published.
const char* kRepo = "exultantliving0/Nintendall-Project";

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
#if defined(_WIN32)
        const size_t width = 400;  // Windows message boxes wrap long lines themselves
#else
        const size_t width = 72;
#endif
        for (const std::string& part : wrapLine(t, width, bullet ? "    " : "")) {
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
    if (!JsonParser(text).parse(list) || list.type != Json::Array) return info;

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

    // The setup file, if GitHub gives its SHA-256 (it's checked before the setup runs).
    const std::string downloads = std::string("https://github.com/") + kRepo + "/releases/download/";
    for (const Json& a : (*best)["assets"].items) {
        const std::string& name = a["name"].s;
        const std::string& url = a["browser_download_url"].s;
        const std::string& digest = a["digest"].s;
        if (name.size() < 10 || name.compare(name.size() - 10, 10, "-setup.exe") != 0) continue;
        if (url.rfind(downloads, 0) != 0 || digest.rfind("sha256:", 0) != 0 || digest.size() != 7 + 64) continue;
        info.setupUrl = url;
        info.setupSha256 = digest.substr(7);
        for (char& c : info.setupSha256) c = char(std::tolower(static_cast<unsigned char>(c)));
        break;
    }
    return info;
}
