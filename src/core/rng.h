#pragma once
// Deterministic RNG.
//
// A single master seed (from the scenario + command stream) derives one
// independent xoshiro256** stream per subsystem. Gameplay code never touches a
// global RNG: it asks for its named stream, so adding a draw in one subsystem can
// never shift the numbers another subsystem sees.

#include <cstdint>
#include <string>

namespace hoi {

enum class RngStream : uint8_t {
    Combat = 0,
    Ai,
    Events,
    Weather,
    Intel,
    Map,
    Count
};

const char* rng_stream_name(RngStream s);
RngStream rng_stream_from_name(const std::string& name, bool* ok);

uint64_t splitmix64(uint64_t& state);

class Rng {
  public:
    Rng() = default;
    explicit Rng(uint64_t seed) { seed_with(seed); }

    void seed_with(uint64_t seed);

    uint64_t next_u64();
    uint32_t next_u32();
    // Uniform in [0, bound); bound must be > 0.
    uint32_t next_below(uint32_t bound);
    // Uniform in [0, 1].
    double next_double();
    // Uniform in [lo, hi].
    double range(double lo, double hi);
    bool chance(double p);

    void serialize_state(uint64_t out[4]) const { for (int i = 0; i < 4; ++i) out[i] = s_[i]; }
    void deserialize_state(const uint64_t in[4]) { for (int i = 0; i < 4; ++i) s_[i] = in[i]; }

  private:
    uint64_t s_[4] = {0x9E3779B97F4A7C15ull, 0xBF58476D1CE4E5B9ull, 0x94D049BB133111EBull,
                      0x2545F4914F6CDD1Dull};
};

class RngSet {
  public:
    void seed(uint64_t master_seed);
    Rng& get(RngStream s) { return streams_[static_cast<int>(s)]; }
    const Rng& get(RngStream s) const { return streams_[static_cast<int>(s)]; }
    uint64_t master_seed() const { return master_seed_; }

    // Hashes the live RNG states so desync checks catch divergence in draws.
    uint64_t state_hash() const;

  private:
    uint64_t master_seed_ = 0;
    Rng streams_[static_cast<int>(RngStream::Count)];
};

}  // namespace hoi
