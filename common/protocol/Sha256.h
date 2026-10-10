#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace p2p {

class Sha256 {
public:
    Sha256();
    void update(const uint8_t* data, size_t len);
    std::array<uint8_t, 32> digest();
    static std::string hex(const std::array<uint8_t, 32>& value);

private:
    void transform(const uint8_t block[64]);
    uint32_t state_[8]{};
    uint64_t bits_ = 0;
    uint8_t buffer_[64]{};
    size_t bufferLen_ = 0;
    bool finished_ = false;
};

}  // namespace p2p
