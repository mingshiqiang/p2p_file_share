#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace p2p {

class ByteWriter {
public:
    explicit ByteWriter(std::vector<uint8_t>& out) : out_(out) {}

    void u8(uint8_t v) { out_.push_back(v); }

    void u16(uint16_t v) {
        out_.push_back(static_cast<uint8_t>(v & 0xff));
        out_.push_back(static_cast<uint8_t>((v >> 8) & 0xff));
    }

    void u32(uint32_t v) {
        for (int i = 0; i < 4; ++i) out_.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
    }

    void u64(uint64_t v) {
        for (int i = 0; i < 8; ++i) out_.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xff));
    }

    void bytes(const void* data, size_t len) {
        auto* p = static_cast<const uint8_t*>(data);
        out_.insert(out_.end(), p, p + len);
    }

    void str(const std::string& s) {
        u16(static_cast<uint16_t>(s.size()));
        bytes(s.data(), s.size());
    }

private:
    std::vector<uint8_t>& out_;
};

class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool ok() const { return ok_; }
    size_t remaining() const { return ok_ && index_ <= size_ ? size_ - index_ : 0; }
    size_t index() const { return index_; }

    uint8_t u8() {
        if (!need(1)) return 0;
        return data_[index_++];
    }

    uint16_t u16() {
        if (!need(2)) return 0;
        uint16_t v = static_cast<uint16_t>(data_[index_]) |
                     (static_cast<uint16_t>(data_[index_ + 1]) << 8);
        index_ += 2;
        return v;
    }

    uint32_t u32() {
        if (!need(4)) return 0;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(data_[index_ + i]) << (8 * i);
        index_ += 4;
        return v;
    }

    uint64_t u64() {
        if (!need(8)) return 0;
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(data_[index_ + i]) << (8 * i);
        index_ += 8;
        return v;
    }

    std::string str(size_t maxLen) {
        uint16_t n = u16();
        if (!ok_) return {};
        if (n > maxLen || !need(n)) {
            ok_ = false;
            return {};
        }
        std::string s(reinterpret_cast<const char*>(data_ + index_), n);
        index_ += n;
        return s;
    }

    bool read(void* dst, size_t n) {
        if (!need(n)) return false;
        std::memcpy(dst, data_ + index_, n);
        index_ += n;
        return true;
    }

    bool skip(size_t n) {
        if (!need(n)) return false;
        index_ += n;
        return true;
    }

private:
    bool need(size_t n) {
        if (!ok_ || n > size_ - index_) {
            ok_ = false;
            return false;
        }
        return true;
    }

    const uint8_t* data_;
    size_t size_;
    size_t index_ = 0;
    bool ok_ = true;
};

}  // namespace p2p
