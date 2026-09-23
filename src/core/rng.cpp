// Deterministic RNG implementation (xoshiro256** + splitmix64 seeding).
//
// Draws are pure functions of the stream state, so the same master seed and the
// same sequence of draws always yields the same numbers on every platform with
// modulo-2^64 arithmetic. No libm, no time, no address values.

#include "core/rng.h"

#include "core/hash.h"

namespace hoi {
namespace {

// Rotation by k (1 <= k <= 63) on a 64-bit word.
inline uint64_t rotl64(uint64_t x, int k) {
    return (x << k) | (x >> (64 - k));
}

}  // namespace

const char* rng_stream_name(RngStream s) {
    switch (s) {
        case RngStream::Combat: return "Combat";
        case RngStream::Ai: return "Ai";
        case RngStream::Events: return "Events";
        case RngStream::Weather: return "Weather";
        case RngStream::Intel: return "Intel";
        case RngStream::Map: return "Map";
        case RngStream::Count: break;
    }
    return "Invalid";
}

RngStream rng_stream_from_name(const std::string& name, bool* ok) {
    for (int i = 0; i < static_cast<int>(RngStream::Count); ++i) {
        const RngStream s = static_cast<RngStream>(i);
        if (name == rng_stream_name(s)) {
            if (ok) *ok = true;
            return s;
        }
    }
    if (ok) *ok = false;
    return RngStream::Combat;
}

uint64_t splitmix64(uint64_t& state) {
    // TestU01 / Vigna reference splitmix64: advance the state by the golden
    // gamma, then two xor-shift-multiply rounds and a final xor-shift.
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

void Rng::seed_with(uint64_t seed) {
    uint64_t sm = seed;
    for (int i = 0; i < 4; ++i) s_[i] = splitmix64(sm);
    // The all-zero state is a fixed point of the xoshiro transition: every draw
    // returns 0 forever. splitmix64 never produces it in practice, but a stream
    // that silently returns zeros would be a determinism trap, so guard.
    if ((s_[0] | s_[1] | s_[2] | s_[3]) == 0) s_[0] = 0x9E3779B97F4A7C15ull;
}

uint64_t Rng::next_u64() {
    const uint64_t result = rotl64(s_[1] * 5, 7) * 9;
    const uint64_t t = s_[1] << 17;
    s_[2] ^= s_[0];
    s_[3] ^= s_[1];
    s_[1] ^= s_[2];
    s_[0] ^= s_[3];
    s_[2] ^= t;
    s_[3] = rotl64(s_[3], 45);
    return result;
}

uint32_t Rng::next_u32() { return static_cast<uint32_t>(next_u64() >> 32); }

uint32_t Rng::next_below(uint32_t bound) {
    // Lemire multiply-shift: for a uniform 32-bit draw x the high 32 bits of
    // x * bound are uniform over [0, bound) up to a 2^-32 bias. Rejection-free,
    // and exactly 0 when bound == 1.
    const uint64_t m = static_cast<uint64_t>(next_u32()) * static_cast<uint64_t>(bound);
    return static_cast<uint32_t>(m >> 32);
}

double Rng::next_double() {
    // 53 significant bits scaled by 2^-53 gives a uniform [0, 1] double; the
    // result is always finite and never negative.
    return static_cast<double>(next_u64() >> 11) * 0x1.0p-53;
}

double Rng::range(double lo, double hi) { return lo + (hi - lo) * next_double(); }

bool Rng::chance(double p) {
    if (p <= 0.0) return false;
    if (p >= 1.0) return true;
    return next_double() < p;
}

void RngSet::seed(uint64_t master_seed) {
    master_seed_ = master_seed;
    for (int i = 0; i < static_cast<int>(RngStream::Count); ++i) {
        // Folding the stream name into the master seed keeps streams independent:
        // adding or reordering a draw in one subsystem cannot shift another's
        // sequence, and a given master seed always reproduces every stream.
        const std::string name = rng_stream_name(static_cast<RngStream>(i));
        uint64_t sm = master_seed ^ fnv1a_str(name);
        streams_[i].seed_with(splitmix64(sm));
    }
}

uint64_t RngSet::state_hash() const {
    Hasher h;
    h.u64(master_seed_);
    for (int i = 0; i < static_cast<int>(RngStream::Count); ++i) {
        uint64_t st[4];
        streams_[i].serialize_state(st);
        for (int j = 0; j < 4; ++j) h.u64(st[j]);
    }
    return h.value();
}

}  // namespace hoi
