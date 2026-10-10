#include "protocol/BleFramer.h"

#include "protocol/Buffer.h"
#include "protocol/Crc16.h"

#include <algorithm>

namespace p2p {

void BleFragmenter::setAttPayload(size_t bytes) {
    if (bytes < kBleHeaderSize + 1) bytes = kDefaultAttPayload;
    if (bytes > 512) bytes = 512;
    attPayload_ = bytes;
}

uint16_t BleFragmenter::nextMsgId() {
    const uint16_t id = nextId_++;
    if (nextId_ == 0) nextId_ = 1;
    return id;
}

std::vector<std::vector<uint8_t>> BleFragmenter::split(SignalType type, uint16_t msgId, const uint8_t* data,
                                                       size_t len) const {
    std::vector<std::vector<uint8_t>> packets;
    if (len > kMaxSignalBytes) return packets;
    const size_t chunk = attPayload_ - kBleHeaderSize;
    size_t count = len == 0 ? 1 : (len + chunk - 1) / chunk;
    if (count == 0) count = 1;
    if (count > 4096) return packets;

    for (size_t i = 0; i < count; ++i) {
        const size_t off = i * chunk;
        const size_t n = len == 0 ? 0 : std::min(chunk, len - off);
        const uint16_t sum = crc16(n ? data + off : nullptr, n);
        std::vector<uint8_t> packet;
        packet.reserve(kBleHeaderSize + n);
        ByteWriter w(packet);
        w.u8(kProtocolVersion);
        w.u8(static_cast<uint8_t>(type));
        w.u16(msgId);
        w.u16(static_cast<uint16_t>(i));
        w.u16(static_cast<uint16_t>(count));
        w.u16(static_cast<uint16_t>(n));
        w.u16(sum);
        if (n) w.bytes(data + off, n);
        packets.push_back(std::move(packet));
    }
    return packets;
}

void BleReassembler::remember(uint16_t msgId) {
    recent_.push_back(msgId);
    if (recent_.size() > 64) recent_.erase(recent_.begin());
}

bool BleReassembler::seen(uint16_t msgId) const {
    return std::find(recent_.begin(), recent_.end(), msgId) != recent_.end();
}

BleReassembler::Completed BleReassembler::push(const uint8_t* packet, size_t len, uint64_t nowMs) {
    Completed done;
    if (len < kBleHeaderSize) {
        done.error = "蓝牙分片过短";
        return done;
    }
    ByteReader r(packet, len);
    const uint8_t version = r.u8();
    const auto type = static_cast<SignalType>(r.u8());
    const uint16_t msgId = r.u16();
    const uint16_t index = r.u16();
    const uint16_t count = r.u16();
    const uint16_t payloadLen = r.u16();
    const uint16_t sum = r.u16();
    if (!r.ok() || version != kProtocolVersion || count == 0 || index >= count || payloadLen != r.remaining()) {
        done.error = "蓝牙分片头无效";
        return done;
    }
    const uint8_t* payload = packet + kBleHeaderSize;
    if (crc16(payloadLen ? payload : nullptr, payloadLen) != sum) {
        done.error = "蓝牙分片校验失败";
        return done;
    }
    if (seen(msgId)) {
        done.duplicate = true;
        return done;
    }

    auto it = std::find_if(pending_.begin(), pending_.end(),
                           [msgId](const Pending& item) { return item.msgId == msgId; });
    if (it == pending_.end()) {
        if (pending_.size() >= 8) {
            done.error = "未完成的蓝牙消息过多";
            return done;
        }
        Pending created;
        created.msgId = msgId;
        created.type = type;
        created.count = count;
        created.startMs = nowMs;
        created.parts.assign(count, {});
        pending_.push_back(std::move(created));
        it = pending_.end() - 1;
    }
    if (it->count != count || it->type != type) {
        done.error = "蓝牙分片与已收到的消息冲突";
        pending_.erase(it);
        return done;
    }
    if (!it->parts[index].empty()) {
        if (it->parts[index].size() != payloadLen ||
            (payloadLen && std::memcmp(it->parts[index].data(), payload, payloadLen) != 0)) {
            done.error = "收到冲突的重复分片";
            pending_.erase(it);
            return done;
        }
        return done;
    }
    it->parts[index].assign(payload, payload + payloadLen);
    ++it->got;
    if (it->got != it->count) return done;

    for (const auto& part : it->parts) done.payload.insert(done.payload.end(), part.begin(), part.end());
    done.ok = true;
    done.type = it->type;
    done.msgId = it->msgId;
    remember(it->msgId);
    pending_.erase(it);
    return done;
}

bool BleReassembler::expire(uint64_t nowMs, std::string* error) {
    bool timedOut = false;
    for (auto it = pending_.begin(); it != pending_.end();) {
        if (nowMs >= it->startMs && nowMs - it->startMs > kReassemblyTimeoutMs) {
            timedOut = true;
            if (error) *error = "蓝牙消息重组超时";
            it = pending_.erase(it);
        } else {
            ++it;
        }
    }
    return timedOut;
}

}  // namespace p2p
