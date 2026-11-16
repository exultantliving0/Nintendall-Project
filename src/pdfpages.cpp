#include "pdfpages.hpp"

#include <zlib.h>

#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

// Just enough of the PDF format to find a scan's page pictures: the objects (also those packed in
// object streams), the page tree, and the pictures each page uses.

namespace {

struct Val {
    enum Type { Null, Num, Name, Ref, Arr, Dict, Other } type = Null;
    double num = 0;
    int ref = 0;
    std::string name;
    std::vector<Val> items;                         // Arr
    std::vector<std::pair<std::string, Val>> keys;  // Dict

    const Val* get(const char* key) const {
        for (const auto& [k, v] : keys)
            if (k == key) return &v;
        return nullptr;
    }
};

bool isSpace(char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t' || c == '\f' || c == '\0'; }
bool isDelim(char c) { return std::strchr("()<>[]{}/%", c) != nullptr && c != '\0'; }
bool isDigit(char c) { return c >= '0' && c <= '9'; }

class Parser {
public:
    Parser(const char* p, const char* end) : p_(p), end_(end) {}
    const char* pos() const { return p_; }

    void skipSpace() {
        while (p_ < end_) {
            if (isSpace(*p_)) ++p_;
            else if (*p_ == '%') {
                while (p_ < end_ && *p_ != '\n' && *p_ != '\r') ++p_;
            } else break;
        }
    }

    bool parse(Val& v, int depth = 0) {
        skipSpace();
        if (p_ >= end_ || depth > 40) return false;
        const char c = *p_;
        if (c == '<' && p_ + 1 < end_ && p_[1] == '<') {
            p_ += 2;
            v.type = Val::Dict;
            for (;;) {
                skipSpace();
                if (p_ + 1 < end_ && p_[0] == '>' && p_[1] == '>') {
                    p_ += 2;
                    return true;
                }
                if (p_ >= end_ || *p_ != '/') return false;
                Val key;
                if (!parse(key, depth + 1)) return false;
                Val value;
                if (!parse(value, depth + 1)) return false;
                v.keys.emplace_back(std::move(key.name), std::move(value));
            }
        }
        if (c == '[') {
            ++p_;
            v.type = Val::Arr;
            for (;;) {
                skipSpace();
                if (p_ < end_ && *p_ == ']') {
                    ++p_;
                    return true;
                }
                Val item;
                if (!parse(item, depth + 1)) return false;
                v.items.push_back(std::move(item));
            }
        }
        if (c == '/') {
            ++p_;
            v.type = Val::Name;
            while (p_ < end_ && !isSpace(*p_) && !isDelim(*p_)) v.name += *p_++;
            return true;
        }
        if (c == '(') {  // a string: skipped
            int level = 0;
            while (p_ < end_) {
                const char ch = *p_++;
                if (ch == '\\') ++p_;
                else if (ch == '(') ++level;
                else if (ch == ')' && --level == 0) break;
            }
            v.type = Val::Other;
            return true;
        }
        if (c == '<') {  // a hex string: skipped
            while (p_ < end_ && *p_ != '>') ++p_;
            ++p_;
            v.type = Val::Other;
            return true;
        }
        if (isDigit(c) || c == '-' || c == '+' || c == '.') {
            const char* start = p_;
            ++p_;
            while (p_ < end_ && (isDigit(*p_) || *p_ == '.')) ++p_;
            v.type = Val::Num;
            v.num = std::atof(std::string(start, p_).c_str());
            // "12 0 R": a reference to object 12.
            const char* save = p_;
            skipSpace();
            const char* g = p_;
            while (p_ < end_ && isDigit(*p_)) ++p_;
            if (p_ > g) {
                skipSpace();
                if (p_ < end_ && *p_ == 'R' && (p_ + 1 >= end_ || isSpace(p_[1]) || isDelim(p_[1]))) {
                    ++p_;
                    v.type = Val::Ref;
                    v.ref = int(v.num);
                    return true;
                }
            }
            p_ = save;
            return true;
        }
        // true, false, null, or something unexpected
        const char* start = p_;
        while (p_ < end_ && !isSpace(*p_) && !isDelim(*p_)) ++p_;
        if (p_ == start) return false;
        v.type = Val::Other;
        return true;
    }

private:
    const char* p_;
    const char* end_;
};

struct Obj {
    Val val;
    bool stream = false;
    size_t start = 0, size = 0;  // the stream's data
};

const char* findText(const char* from, const char* end, const char* text) {
    const size_t n = std::strlen(text);
    if (end - from < std::ptrdiff_t(n)) return nullptr;
    const char* it = std::search(from, end, text, text + n);
    return it == end ? nullptr : it;
}

bool inflateAll(const uint8_t* data, size_t size, std::string& out) {
    z_stream z{};
    if (inflateInit(&z) != Z_OK) return false;
    z.next_in = const_cast<Bytef*>(data);
    z.avail_in = uInt(size);
    char buf[65536];
    int r = Z_OK;
    while (r == Z_OK) {
        z.next_out = reinterpret_cast<Bytef*>(buf);
        z.avail_out = sizeof buf;
        r = inflate(&z, Z_NO_FLUSH);
        out.append(buf, sizeof buf - z.avail_out);
        if (out.size() > (64u << 20)) break;
    }
    inflateEnd(&z);
    return r == Z_STREAM_END || !out.empty();
}

class Pdf {
public:
    explicit Pdf(const std::vector<uint8_t>& bytes) : b_(reinterpret_cast<const char*>(bytes.data())), n_(bytes.size()) {
        readObjects();
        unpackObjectStreams();
    }

    const Val& resolve(const Val& v, int depth = 0) const {
        static const Val none;
        if (v.type != Val::Ref) return v;
        if (depth > 20) return none;
        const auto it = objs_.find(v.ref);
        return it == objs_.end() ? none : resolve(it->second.val, depth + 1);
    }
    const Obj* object(const Val& v) const {
        if (v.type != Val::Ref) return nullptr;
        const auto it = objs_.find(v.ref);
        return it == objs_.end() ? nullptr : &it->second;
    }
    const Val* get(const Val& dict, const char* key) const {
        const Val* v = resolve(dict).get(key);
        return v ? &resolve(*v) : nullptr;
    }
    bool isType(const Val& dict, const char* key, const char* name) const {
        const Val* v = get(dict, key);
        return v && v->type == Val::Name && v->name == name;
    }
    double number(const Val& dict, const char* key) const {
        const Val* v = get(dict, key);
        return v && v->type == Val::Num ? v->num : 0;
    }

    // The page tree's pages, in order (each with the resources it inherits).
    std::vector<std::pair<const Val*, const Val*>> pages() const {
        std::vector<std::pair<const Val*, const Val*>> out;
        const Val* root = nullptr;
        for (const auto& [num, o] : objs_)
            if (isType(o.val, "Type", "Catalog")) root = get(o.val, "Pages");
        if (!root) {
            for (const auto& [num, o] : objs_)
                if (isType(o.val, "Type", "Pages") && !o.val.get("Parent")) root = &o.val;
        }
        if (root) {
            std::set<const Val*> seen;
            walk(*root, nullptr, 0, seen, out);
        }
        return out;
    }

    // The names of the pictures a page draws ("/Im3 Do" in its contents). Empty if they can't be read.
    std::set<std::string> drawn(const Val& page) const {
        std::set<std::string> names;
        const Val* contents = resolve(page).get("Contents");
        if (!contents) return names;
        std::vector<const Val*> parts;
        const Val& c = resolve(*contents);
        if (c.type == Val::Arr) {
            for (const Val& v : c.items) parts.push_back(&v);
        } else {
            parts.push_back(contents);
        }
        for (const Val* part : parts) {
            const Obj* o = object(*part);
            if (!o || !o->stream) continue;
            std::string data;
            const Val* filter = get(o->val, "Filter");
            if (filter && filter->type == Val::Arr && filter->items.size() == 1) filter = &resolve(filter->items[0]);
            if (!filter) data.assign(b_ + o->start, o->size);
            else if (filter->type == Val::Name && filter->name == "FlateDecode")
                inflateAll(reinterpret_cast<const uint8_t*>(b_ + o->start), o->size, data);
            else
                return {};
            for (size_t at = data.find("Do"); at != std::string::npos; at = data.find("Do", at + 2)) {
                if (at + 2 < data.size() && !isSpace(data[at + 2]) && !isDelim(data[at + 2])) continue;
                size_t e = at;
                while (e > 0 && isSpace(data[e - 1])) --e;
                size_t b = e;
                while (b > 0 && !isSpace(data[b - 1]) && !isDelim(data[b - 1])) --b;
                if (b > 0 && data[b - 1] == '/' && b < e) names.insert(data.substr(b, e - b));
            }
        }
        return names;
    }

    // The biggest JPEG picture a page (or a form on it) draws, and the biggest other picture.
    // used: the names the page draws (nullptr or empty = every picture in its resources).
    void pictures(const Val* resources, int depth, const std::set<std::string>* used, const Obj*& bestJpeg,
                  double& jpegArea, double& otherArea) const {
        if (!resources || depth > 3) return;
        const Val* xobjects = get(*resources, "XObject");
        if (!xobjects || xobjects->type != Val::Dict) return;
        for (const auto& [name, ref] : xobjects->keys) {
            if (used && !used->empty() && !used->count(name)) continue;
            const Obj* o = object(ref);
            if (!o || !o->stream) continue;
            if (isType(o->val, "Subtype", "Form")) {
                pictures(get(o->val, "Resources"), depth + 1, nullptr, bestJpeg, jpegArea, otherArea);
                continue;
            }
            if (!isType(o->val, "Subtype", "Image")) continue;
            const double area = number(o->val, "Width") * number(o->val, "Height");
            const Val* filter = get(o->val, "Filter");
            if (filter && filter->type == Val::Arr && filter->items.size() == 1) filter = &resolve(filter->items[0]);
            const bool jpeg = filter && filter->type == Val::Name && (filter->name == "DCTDecode" || filter->name == "DCT");
            const bool looksJpeg = o->size > 3 && uint8_t(b_[o->start]) == 0xFF && uint8_t(b_[o->start + 1]) == 0xD8;
            if (jpeg && looksJpeg) {
                if (area > jpegArea) {
                    jpegArea = area;
                    bestJpeg = o;
                }
            } else {
                otherArea = std::max(otherArea, area);
            }
        }
    }

private:
    void walk(const Val& node, const Val* resources, int depth, std::set<const Val*>& seen,
              std::vector<std::pair<const Val*, const Val*>>& out) const {
        const Val& n = resolve(node);
        if (n.type != Val::Dict || depth > 30 || !seen.insert(&n).second || out.size() > 5000) return;
        if (const Val* r = get(n, "Resources")) resources = r;
        const Val* kids = get(n, "Kids");
        if (kids && kids->type == Val::Arr && !isType(n, "Type", "Page")) {
            for (const Val& k : kids->items) walk(k, resources, depth + 1, seen, out);
        } else {
            out.emplace_back(&n, resources);
        }
    }

    // Every "12 0 obj ... endobj" in the file (a later one with the same number wins: an update).
    void readObjects() {
        const char* end = b_ + n_;
        const char* p = b_;
        while (const char* o = findText(p, end, "obj")) {
            p = o + 3;
            if (p < end && !isSpace(*p) && !isDelim(*p)) continue;  // "objects"...
            // Before it: "<number> <generation> "
            const char* q = o;
            if (q == b_ || !isSpace(q[-1])) continue;
            while (q > b_ && isSpace(q[-1])) --q;
            const char* g = q;
            while (q > b_ && isDigit(q[-1])) --q;
            if (q == g) continue;
            while (q > b_ && isSpace(q[-1])) --q;
            const char* numEnd = q;
            while (q > b_ && isDigit(q[-1])) --q;
            if (q == numEnd || (q > b_ && !isSpace(q[-1]) && !isDelim(q[-1]))) continue;
            const int num = std::atoi(std::string(q, numEnd).c_str());

            Parser parser(p, end);
            Obj obj;
            if (!parser.parse(obj.val)) continue;
            p = parser.pos();
            parser.skipSpace();
            const char* s = parser.pos();
            if (end - s > 6 && std::memcmp(s, "stream", 6) == 0) {
                s += 6;
                if (s < end && *s == '\r') ++s;
                if (s < end && *s == '\n') ++s;
                obj.stream = true;
                obj.start = size_t(s - b_);
                // The length, if it's written there and right; else up to "endstream".
                const Val* len = obj.val.get("Length");
                size_t size = 0;
                if (len && len->type == Val::Num && len->num > 0 && s + size_t(len->num) <= end) {
                    const char* e = s + size_t(len->num);
                    while (e < end && isSpace(*e)) ++e;
                    if (end - e >= 9 && std::memcmp(e, "endstream", 9) == 0) size = size_t(len->num);
                }
                if (size == 0) {
                    const char* e = findText(s, end, "endstream");
                    if (!e) break;
                    const char* t = e;
                    if (t > s && t[-1] == '\n') --t;
                    if (t > s && t[-1] == '\r') --t;
                    size = size_t(t - s);
                }
                obj.size = size;
                p = s + size;
            }
            objs_[num] = std::move(obj);
        }
    }

    // Objects packed into compressed object streams (PDF 1.5 and later).
    void unpackObjectStreams() {
        std::vector<int> streams;
        for (const auto& [num, o] : objs_)
            if (o.stream && isType(o.val, "Type", "ObjStm")) streams.push_back(num);
        for (const int num : streams) {
            const Obj& o = objs_[num];
            const Val* filter = get(o.val, "Filter");
            if (filter && filter->type == Val::Arr && filter->items.size() == 1) filter = &resolve(filter->items[0]);
            if (filter && !(filter->type == Val::Name && filter->name == "FlateDecode")) continue;
            std::string data;
            if (filter) {
                if (!inflateAll(reinterpret_cast<const uint8_t*>(b_ + o.start), o.size, data)) continue;
            } else {
                data.assign(b_ + o.start, o.size);
            }
            const int count = int(number(o.val, "N"));
            const size_t first = size_t(number(o.val, "First"));
            if (count <= 0 || first >= data.size()) continue;
            packed_.push_back(std::move(data));
            const std::string& d = packed_.back();
            Parser header(d.data(), d.data() + first);
            std::vector<std::pair<int, size_t>> where;
            for (int i = 0; i < count; ++i) {
                Val a, b;
                if (!header.parse(a) || !header.parse(b) || a.type != Val::Num || b.type != Val::Num) break;
                where.emplace_back(int(a.num), first + size_t(b.num));
            }
            for (const auto& [n, at] : where) {
                if (at >= d.size() || objs_.count(n)) continue;
                Parser parser(d.data() + at, d.data() + d.size());
                Obj packed;
                if (parser.parse(packed.val)) objs_[n] = std::move(packed);
            }
        }
    }

    const char* b_;
    size_t n_;
    std::unordered_map<int, Obj> objs_;
    std::vector<std::string> packed_;
};

}  // namespace

bool pdfJpegPages(const std::vector<uint8_t>& bytes, std::vector<PdfPicture>& out) {
    out.clear();
    if (bytes.size() < 16 || std::memcmp(bytes.data(), "%PDF", 4) != 0) return false;
    const Pdf pdf(bytes);
    int found = 0;
    for (const auto& [page, resources] : pdf.pages()) {
        const Obj* best = nullptr;
        double jpegArea = 0, otherArea = 0;
        const std::set<std::string> used = pdf.drawn(*page);
        pdf.pictures(resources, 0, &used, best, jpegArea, otherArea);
        PdfPicture pic;
        // A page whose main picture isn't a JPEG (a JPEG 2000 scan with a small JPEG on it) can't be shown.
        if (best && otherArea <= jpegArea * 1.5) {
            pic.offset = best->start;
            pic.size = best->size;
            ++found;
        }
        out.push_back(pic);
    }
    // Most pages must be pictures (a page or two may be something else: shown as "can't be shown").
    if (out.empty() || found * 5 < int(out.size()) * 4) {
        out.clear();
        return false;
    }
    return true;
}
