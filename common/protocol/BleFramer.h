#pragma once

#include "protocol/Ids.h"

#include <cstdint>
#include <string>
#include <vector>

namespace p2p {

class BleFragmenter {
public:
    void setAttPayload(size_t bytes);
    size_t attPayload() const { return attPayload_; }

    // 把一条完整信令切成若干 GATT 包。单包长度不超过当前 ATT 载荷。
    std::vector<std::vector<uint8_t>> split(SignalType type, uint16_t msgId, const uint8_t* data, size_t len) const;
    uint16_t nextMsgId();

private:
    size_t attPayload_ = kDefaultAttPayload;
    uint16_t nextId_ = 1;
};

class BleReassembler {
public:
    struct Completed {
        bool ok = false;
        bool duplicate = false;
        std::string error;
        SignalType type = SignalType::Error;
        uint16_t msgId = 0;
        std::vector<uint8_t> payload;
    };

    Completed push(const uint8_t* packet, size_t len, uint64_t nowMs);
    // 丢弃超时未完成的消息。发生超时时写入 error。
    bool expire(uint64_t nowMs, std::string* error);

private:
    struct Pending {
        uint16_t msgId = 0;
        SignalType type = SignalType::Error;
        uint16_t count = 0;
        uint64_t startMs = 0;
        std::vector<std::vector<uint8_t>> parts;
        int got = 0;
    };

    std::vector<Pending> pending_;
    std::vector<uint16_t> recent_;
    void remember(uint16_t msgId);
    bool seen(uint16_t msgId) const;
};

}  // namespace p2p
