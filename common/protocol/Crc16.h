#pragma once

#include <cstddef>
#include <cstdint>

namespace p2p {

// CRC-16/CCITT-FALSE：多项式 0x1021，初始值 0xFFFF，无反射，无最终异或。
uint16_t crc16(const uint8_t* data, size_t len);

}  // namespace p2p
