#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

// Rewind history (hold ` to rewind, like Mesen): the recent past as a chain of save states.
// Only the newest state is kept whole; each older one is stored as its difference from the
// next newer one (mostly zeros, packed small), so minutes of play fit in a few hundred MB.
class Rewind {
public:
    void clear();
    void setLimits(size_t maxBytes, size_t maxStates) { maxBytes_ = maxBytes; maxStates_ = maxStates; }
    // What the game read from the controllers during one frame, so a rewound moment can be
    // shown exactly as it was played.
    struct InputRead {
        uint8_t port, index;
        uint16_t device, id;
        int16_t value;
    };
    using Inputs = std::vector<InputRead>;

    // Adds the newest state, and the controller reads of the frame that led to it. Takes the
    // buffer's contents (it's swapped, so the caller can reuse it).
    void push(std::vector<uint8_t>& state, Inputs inputs);
    // Steps back: `state` gets the state before the newest one, which then becomes the newest;
    // `inputs` gets the controller reads that led from it to the dropped one.
    // False when the history is used up.
    bool pop(std::vector<uint8_t>& state, Inputs& inputs);
    bool empty() const { return older_.empty(); }
    size_t bytes() const { return bytes_; }

private:
    struct Diff {
        std::vector<uint8_t> data;  // packed XOR of the older and the newer state
        size_t olderSize = 0;
        Inputs olderInputs;
    };
    std::vector<uint8_t> newest_;
    Inputs newestInputs_;
    std::deque<Diff> older_;  // oldest first
    std::vector<uint8_t> scratch_;
    size_t bytes_ = 0;
    size_t maxBytes_ = 256u << 20;
    size_t maxStates_ = 18000;
};
