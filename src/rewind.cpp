#include "rewind.hpp"

#include <algorithm>
#include <cstring>

namespace {

void put32(std::vector<uint8_t>& out, uint32_t v) {
    const uint8_t b[4] = {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)};
    out.insert(out.end(), b, b + 4);
}

uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

uint64_t load64(const uint8_t* p) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

// Packs a XOR b (the shorter one padded with zeros) as runs: [zero count][byte count][bytes]...
void encode(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, std::vector<uint8_t>& out) {
    out.clear();
    const size_t na = a.size(), nb = b.size(), n = std::max(na, nb), m = std::min(na, nb);
    auto x = [&](size_t i) -> uint8_t { return uint8_t((i < na ? a[i] : 0) ^ (i < nb ? b[i] : 0)); };
    size_t i = 0;
    while (i < n) {
        // Unchanged bytes (8 at a time while both states have them).
        size_t z = i;
        while (z + 8 <= m && load64(&a[z]) == load64(&b[z])) z += 8;
        while (z < n && x(z) == 0) ++z;
        // Changed bytes, up to the next run of 16 unchanged ones.
        size_t end = z;
        if (z < n) {
            size_t lastChanged = z;
            for (size_t e = z; e < n && e - lastChanged < 16; ++e)
                if (x(e)) lastChanged = e;
            end = lastChanged + 1;
        }
        put32(out, uint32_t(z - i));
        put32(out, uint32_t(end - z));
        for (size_t k = z; k < end; ++k) out.push_back(x(k));
        i = end;
    }
}

// state ^= packed difference.
void applyDiff(const std::vector<uint8_t>& diff, std::vector<uint8_t>& state) {
    size_t at = 0;
    for (size_t p = 0; p + 8 <= diff.size();) {
        at += get32(&diff[p]);
        const uint32_t len = get32(&diff[p + 4]);
        p += 8;
        if (at + len > state.size()) state.resize(at + len, 0);
        for (uint32_t k = 0; k < len && p < diff.size(); ++k) state[at++] ^= diff[p++];
    }
}

}  // namespace

void Rewind::clear() {
    newest_.clear();
    newestInputs_.clear();
    older_.clear();
    bytes_ = 0;
}

void Rewind::push(std::vector<uint8_t>& state, Inputs inputs) {
    if (!newest_.empty()) {
        encode(newest_, state, scratch_);
        Diff d;
        d.data.assign(scratch_.begin(), scratch_.end());  // exact size
        d.olderSize = newest_.size();
        d.olderInputs = std::move(newestInputs_);
        bytes_ += d.data.size() + d.olderInputs.size() * sizeof(InputRead);
        older_.push_back(std::move(d));
    }
    newest_.swap(state);
    newestInputs_ = std::move(inputs);
    // Forget the oldest moments when the history is full.
    while (!older_.empty() && (bytes_ > maxBytes_ || older_.size() > maxStates_)) {
        bytes_ -= older_.front().data.size() + older_.front().olderInputs.size() * sizeof(InputRead);
        older_.pop_front();
    }
}

bool Rewind::pop(std::vector<uint8_t>& state, Inputs& inputs) {
    if (older_.empty()) return false;
    Diff& d = older_.back();
    applyDiff(d.data, newest_);
    newest_.resize(d.olderSize);
    inputs = std::move(newestInputs_);
    newestInputs_ = std::move(d.olderInputs);
    bytes_ -= d.data.size() + newestInputs_.size() * sizeof(InputRead);
    older_.pop_back();
    state = newest_;
    return true;
}
