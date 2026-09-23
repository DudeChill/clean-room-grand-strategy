#pragma once
// FNV-1a 64-bit hashing used for world hashes, subsystem hashes and desync
// diagnostics. Hashes are computed over the same canonical byte stream that
// serialization produces, so "hash equal" and "state equal" mean the same thing.

#include <cstdint>
#include <cstring>
#include <string>

namespace hoi {

inline constexpr uint64_t FNV_OFFSET = 0xCBF29CE484222325ull;
inline constexpr uint64_t FNV_PRIME = 0x100000001B3ull;

class Hasher {
  public:
    Hasher() = default;
    explicit Hasher(uint64_t seed) : h_(seed) {}

    void bytes(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < n; ++i) {
            h_ ^= p[i];
            h_ *= FNV_PRIME;
        }
    }
    void u8(uint8_t v) { bytes(&v, 1); }
    void u32(uint32_t v) { bytes(&v, sizeof(v)); }
    void u64(uint64_t v) { bytes(&v, sizeof(v)); }
    void i32(int32_t v) { bytes(&v, sizeof(v)); }
    void i64(int64_t v) { bytes(&v, sizeof(v)); }
    void f64(double v) {
        uint64_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        u64(bits);
    }
    void str(const std::string& s) {
        u32(static_cast<uint32_t>(s.size()));
        bytes(s.data(), s.size());
    }
    void bool_(bool b) { u8(b ? 1 : 0); }

    [[nodiscard]] uint64_t value() const { return h_; }

  private:
    uint64_t h_ = FNV_OFFSET;
};

inline uint64_t fnv1a(const void* data, size_t n) {
    Hasher h;
    h.bytes(data, n);
    return h.value();
}

inline uint64_t fnv1a_str(const std::string& s) { return fnv1a(s.data(), s.size()); }

}  // namespace hoi
