#pragma once
// Canonical binary encoding. Doubles are written as their IEEE-754 bit pattern so
// that save/load and hashing are bit-exact, never decimal-approximate.
// The encoding is little-endian and self-describing only at the container level
// (save files carry version + section tags); payload layout is defined by the
// structures that write it.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace hoi {

class ByteWriter {
  public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u16(uint16_t v) {
        buf_.push_back(static_cast<uint8_t>(v & 0xFF));
        buf_.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    }
    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) buf_.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) buf_.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void i64(int64_t v) { u64(static_cast<uint64_t>(v)); }
    void f64(double v) {
        uint64_t bits;
        std::memcpy(&bits, &v, sizeof(bits));
        u64(bits);
    }
    void boolean(bool b) { u8(b ? 1 : 0); }
    void str(const std::string& s) {
        u32(static_cast<uint32_t>(s.size()));
        raw(s.data(), s.size());
    }
    void raw(const void* data, size_t n) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        buf_.insert(buf_.end(), p, p + n);
    }
    template <typename T>
    void vec(const std::vector<T>& v) {
        u32(static_cast<uint32_t>(v.size()));
        for (const auto& e : v) *this << e;
    }

    [[nodiscard]] const std::vector<uint8_t>& data() const { return buf_; }
    [[nodiscard]] size_t size() const { return buf_.size(); }
    void clear() { buf_.clear(); }

  private:
    std::vector<uint8_t> buf_;
};

// Convenience operators keep serializers compact: `w << x;` for scalars.
inline ByteWriter& operator<<(ByteWriter& w, uint8_t v) { w.u8(v); return w; }
inline ByteWriter& operator<<(ByteWriter& w, uint16_t v) { w.u16(v); return w; }
inline ByteWriter& operator<<(ByteWriter& w, uint32_t v) { w.u32(v); return w; }
inline ByteWriter& operator<<(ByteWriter& w, uint64_t v) { w.u64(v); return w; }
inline ByteWriter& operator<<(ByteWriter& w, int32_t v) { w.i32(v); return w; }
inline ByteWriter& operator<<(ByteWriter& w, int64_t v) { w.i64(v); return w; }
inline ByteWriter& operator<<(ByteWriter& w, double v) { w.f64(v); return w; }
inline ByteWriter& operator<<(ByteWriter& w, bool v) { w.boolean(v); return w; }
inline ByteWriter& operator<<(ByteWriter& w, const std::string& v) { w.str(v); return w; }

class ByteReader {
  public:
    ByteReader(const uint8_t* data, size_t n) : p_(data), end_(data + n) {}
    explicit ByteReader(const std::vector<uint8_t>& v) : p_(v.data()), end_(v.data() + v.size()) {}

    bool u8(uint8_t* out) {
        if (p_ + 1 > end_) return fail();
        *out = *p_++;
        return true;
    }
    bool u16(uint16_t* out) {
        if (p_ + 2 > end_) return fail();
        *out = static_cast<uint16_t>(p_[0] | (p_[1] << 8));
        p_ += 2;
        return true;
    }
    bool u32(uint32_t* out) {
        if (p_ + 4 > end_) return fail();
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p_[i]) << (8 * i);
        p_ += 4;
        *out = v;
        return true;
    }
    bool u64(uint64_t* out) {
        if (p_ + 8 > end_) return fail();
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p_[i]) << (8 * i);
        p_ += 8;
        *out = v;
        return true;
    }
    bool i32(int32_t* out) {
        uint32_t v;
        if (!u32(&v)) return false;
        std::memcpy(out, &v, sizeof(v));
        return true;
    }
    bool i64(int64_t* out) {
        uint64_t v;
        if (!u64(&v)) return false;
        std::memcpy(out, &v, sizeof(v));
        return true;
    }
    bool f64(double* out) {
        uint64_t bits;
        if (!u64(&bits)) return false;
        std::memcpy(out, &bits, sizeof(bits));
        return true;
    }
    bool boolean(bool* out) {
        uint8_t v;
        if (!u8(&v)) return false;
        *out = v != 0;
        return true;
    }
    bool str(std::string* out) {
        uint32_t n;
        if (!u32(&n)) return false;
        if (p_ + n > end_) return fail();
        out->assign(reinterpret_cast<const char*>(p_), n);
        p_ += n;
        return true;
    }
    bool raw(void* dst, size_t n) {
        if (p_ + n > end_) return fail();
        std::memcpy(dst, p_, n);
        p_ += n;
        return true;
    }

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] bool eof() const { return p_ == end_; }
    [[nodiscard]] size_t remaining() const { return static_cast<size_t>(end_ - p_); }

  private:
    bool fail() {
        ok_ = false;
        return false;
    }

    const uint8_t* p_;
    const uint8_t* end_;
    bool ok_ = true;
};

inline ByteReader& operator>>(ByteReader& r, uint8_t& v) { r.u8(&v); return r; }
inline ByteReader& operator>>(ByteReader& r, uint16_t& v) { r.u16(&v); return r; }
inline ByteReader& operator>>(ByteReader& r, uint32_t& v) { r.u32(&v); return r; }
inline ByteReader& operator>>(ByteReader& r, uint64_t& v) { r.u64(&v); return r; }
inline ByteReader& operator>>(ByteReader& r, int32_t& v) { r.i32(&v); return r; }
inline ByteReader& operator>>(ByteReader& r, int64_t& v) { r.i64(&v); return r; }
inline ByteReader& operator>>(ByteReader& r, double& v) { r.f64(&v); return r; }
inline ByteReader& operator>>(ByteReader& r, bool& v) { r.boolean(&v); return r; }
inline ByteReader& operator>>(ByteReader& r, std::string& v) { r.str(&v); return r; }

}  // namespace hoi
