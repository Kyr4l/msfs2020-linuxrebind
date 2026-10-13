#pragma once
// Little-endian serialization helpers for the SimConnect wire protocol.
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace lr::sim {

class ByteWriter {
public:
    void u8(uint8_t v) { buf_.push_back(v); }
    void u32(uint32_t v) { raw(&v, 4); }
    void i32(int32_t v) { raw(&v, 4); }
    void u64(uint64_t v) { raw(&v, 8); }
    void f32(float v) { raw(&v, 4); }
    void f64(double v) { raw(&v, 8); }
    void bytes(const void* p, size_t n) { raw(p, n); }
    // Writes the string and pads with zeros up to `fixed` bytes (a longer string is written in full).
    void str(const std::string& s, size_t fixed) {
        raw(s.data(), s.size());
        if (s.size() < fixed) buf_.insert(buf_.end(), fixed - s.size(), 0);
    }
    void u32At(size_t off, uint32_t v) { std::memcpy(buf_.data() + off, &v, 4); }
    size_t size() const { return buf_.size(); }
    const std::vector<uint8_t>& data() const { return buf_; }

private:
    void raw(const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        buf_.insert(buf_.end(), b, b + n);
    }
    std::vector<uint8_t> buf_;
};

class ByteReader {
public:
    ByteReader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    uint32_t u32() { uint32_t v = 0; take(&v, 4); return v; }
    int32_t i32() { int32_t v = 0; take(&v, 4); return v; }
    uint64_t u64() { uint64_t v = 0; take(&v, 8); return v; }
    float f32() { float v = 0; take(&v, 4); return v; }
    double f64() { double v = 0; take(&v, 8); return v; }
    std::string str(size_t fixed) {
        if (off_ + fixed > n_) { ok_ = false; off_ = n_; return {}; }
        const char* s = reinterpret_cast<const char*>(p_ + off_);
        size_t len = strnlen(s, fixed);
        off_ += fixed;
        return std::string(s, len);
    }
    void skip(size_t n) { off_ = off_ + n > n_ ? n_ : off_ + n; }
    size_t remaining() const { return n_ - off_; }
    const uint8_t* cursor() const { return p_ + off_; }
    bool ok() const { return ok_; }

private:
    void take(void* dst, size_t n) {
        if (off_ + n > n_) { ok_ = false; std::memset(dst, 0, n); off_ = n_; return; }
        std::memcpy(dst, p_ + off_, n);
        off_ += n;
    }
    const uint8_t* p_;
    size_t n_;
    size_t off_ = 0;
    bool ok_ = true;
};

}  // namespace lr::sim
