#include "protocol/FileFrame.h"

#include "protocol/Buffer.h"

#include <cstring>

namespace p2p {

bool TransferId::operator==(const TransferId& other) const {
    return std::memcmp(bytes, other.bytes, sizeof(bytes)) == 0;
}

bool TransferId::empty() const {
    for (uint8_t b : bytes) {
        if (b) return false;
    }
    return true;
}

std::string TransferId::hex() const {
    static const char* digits = "0123456789abcdef";
    std::string out(32, '0');
    for (size_t i = 0; i < 16; ++i) {
        out[i * 2] = digits[bytes[i] >> 4];
        out[i * 2 + 1] = digits[bytes[i] & 0x0f];
    }
    return out;
}

bool encodeFileFrame(const FileFrame& frame, std::vector<uint8_t>* out, std::string* error) {
    if (!out) return false;
    if (kFileHeaderSize + frame.body.size() > kMaxDatagram) {
        if (error) *error = "数据报超过 1200 字节";
        return false;
    }
    out->clear();
    ByteWriter w(*out);
    w.bytes(kFileMagic, 4);
    w.u8(kProtocolVersion);
    w.u8(static_cast<uint8_t>(frame.type));
    w.u8(frame.flags);
    w.u8(static_cast<uint8_t>(kFileHeaderSize));
    w.bytes(frame.id.bytes, 16);
    w.bytes(frame.body.data(), frame.body.size());
    return true;
}

bool decodeFileFrame(const uint8_t* data, size_t len, FileFrame* out, std::string* error) {
    if (!out || len < kFileHeaderSize || len > kMaxDatagram) {
        if (error) *error = "文件帧长度无效";
        return false;
    }
    if (std::memcmp(data, kFileMagic, 4) != 0) {
        if (error) *error = "文件帧魔数不匹配";
        return false;
    }
    ByteReader r(data, len);
    r.skip(4);
    const uint8_t version = r.u8();
    const auto type = static_cast<FileMsgType>(r.u8());
    const uint8_t flags = r.u8();
    const uint8_t headerLen = r.u8();
    if (!r.ok() || version != kProtocolVersion || flags != 0 || headerLen < kFileHeaderSize || headerLen > len) {
        if (error) *error = "文件帧头无效";
        return false;
    }
    if (!r.read(out->id.bytes, 16)) {
        if (error) *error = "传输 ID 无效";
        return false;
    }
    if (headerLen > kFileHeaderSize && !r.skip(headerLen - kFileHeaderSize)) {
        if (error) *error = "文件帧头扩展无效";
        return false;
    }
    const auto raw = static_cast<uint16_t>(type);
    if (raw < 1 || raw > 12) {
        if (error) *error = "未知的文件消息类型";
        return false;
    }
    out->type = type;
    out->flags = flags;
    out->body.assign(data + headerLen, data + len);
    return true;
}

bool encodeOffer(const std::string& fileName, uint64_t fileSize, std::vector<uint8_t>* body, std::string* error) {
    if (!body || fileName.size() > 180) {
        if (error) *error = "文件名无法放入文件请求";
        return false;
    }
    body->clear();
    ByteWriter w(*body);
    w.u64(fileSize);
    w.str(fileName);
    return true;
}

bool decodeOffer(const uint8_t* data, size_t len, std::string* fileName, uint64_t* fileSize, std::string* error) {
    ByteReader r(data, len);
    const uint64_t size = r.u64();
    const std::string name = r.str(180);
    if (!r.ok() || r.remaining() != 0 || !fileName || !fileSize) {
        if (error) *error = "文件请求格式错误";
        return false;
    }
    *fileName = name;
    *fileSize = size;
    return true;
}

bool encodeReject(uint16_t code, const std::string& message, std::vector<uint8_t>* body, std::string* error) {
    if (!body || message.size() > 400) {
        if (error) *error = "拒绝原因过长";
        return false;
    }
    body->clear();
    ByteWriter w(*body);
    w.u16(code);
    w.str(message);
    return true;
}

bool decodeReject(const uint8_t* data, size_t len, uint16_t* code, std::string* message, std::string* error) {
    ByteReader r(data, len);
    const uint16_t c = r.u16();
    const std::string text = r.str(400);
    if (!r.ok() || r.remaining() != 0 || !code || !message) {
        if (error) *error = "拒绝消息格式错误";
        return false;
    }
    *code = c;
    *message = text;
    return true;
}

void encodeChunk(uint32_t seq, uint64_t offset, const uint8_t* data, uint16_t len, std::vector<uint8_t>* body) {
    body->clear();
    ByteWriter w(*body);
    w.u32(seq);
    w.u64(offset);
    w.u16(len);
    if (len && data) w.bytes(data, len);
}

bool decodeChunk(const uint8_t* data, size_t len, uint32_t* seq, uint64_t* offset, std::vector<uint8_t>* payload,
                 std::string* error) {
    ByteReader r(data, len);
    const uint32_t s = r.u32();
    const uint64_t off = r.u64();
    const uint16_t n = r.u16();
    if (!r.ok() || n > kChunkPayload || r.remaining() != n || !seq || !offset || !payload) {
        if (error) *error = "文件分块格式错误";
        return false;
    }
    payload->resize(n);
    if (n && !r.read(payload->data(), n)) {
        if (error) *error = "文件分块数据被截断";
        return false;
    }
    *seq = s;
    *offset = off;
    return true;
}

void encodeAck(uint32_t nextExpected, std::vector<uint8_t>* body) {
    body->clear();
    ByteWriter w(*body);
    w.u32(nextExpected);
}

bool decodeAck(const uint8_t* data, size_t len, uint32_t* nextExpected, std::string* error) {
    ByteReader r(data, len);
    const uint32_t v = r.u32();
    if (!r.ok() || r.remaining() != 0 || !nextExpected) {
        if (error) *error = "确认消息格式错误";
        return false;
    }
    *nextExpected = v;
    return true;
}

void encodeEnd(uint64_t fileSize, uint32_t totalChunks, const std::array<uint8_t, 32>& hash, std::vector<uint8_t>* body) {
    body->clear();
    ByteWriter w(*body);
    w.u64(fileSize);
    w.u32(totalChunks);
    w.bytes(hash.data(), hash.size());
}

bool decodeEnd(const uint8_t* data, size_t len, uint64_t* fileSize, uint32_t* totalChunks, std::array<uint8_t, 32>* hash,
               std::string* error) {
    ByteReader r(data, len);
    const uint64_t size = r.u64();
    const uint32_t chunks = r.u32();
    std::array<uint8_t, 32> digest{};
    if (!r.read(digest.data(), digest.size()) || r.remaining() != 0 || !fileSize || !totalChunks || !hash) {
        if (error) *error = "结束消息格式错误";
        return false;
    }
    *fileSize = size;
    *totalChunks = chunks;
    *hash = digest;
    return true;
}

void encodeHash(const std::array<uint8_t, 32>& hash, std::vector<uint8_t>* body) {
    body->clear();
    body->insert(body->end(), hash.begin(), hash.end());
}

bool decodeHash(const uint8_t* data, size_t len, std::array<uint8_t, 32>* hash, std::string* error) {
    if (!hash || len != 32) {
        if (error) *error = "校验值长度错误";
        return false;
    }
    std::memcpy(hash->data(), data, 32);
    return true;
}

void encodeStart(uint64_t fileSize, uint32_t chunkSize, std::vector<uint8_t>* body) {
    body->clear();
    ByteWriter w(*body);
    w.u64(fileSize);
    w.u32(chunkSize);
}

bool decodeStart(const uint8_t* data, size_t len, uint64_t* fileSize, uint32_t* chunkSize, std::string* error) {
    ByteReader r(data, len);
    const uint64_t size = r.u64();
    const uint32_t chunk = r.u32();
    if (!r.ok() || r.remaining() != 0 || !fileSize || !chunkSize) {
        if (error) *error = "开始消息格式错误";
        return false;
    }
    *fileSize = size;
    *chunkSize = chunk;
    return true;
}

void encodePing(uint64_t timestamp, std::vector<uint8_t>* body) {
    body->clear();
    ByteWriter w(*body);
    w.u64(timestamp);
}

bool decodePing(const uint8_t* data, size_t len, uint64_t* timestamp, std::string* error) {
    ByteReader r(data, len);
    const uint64_t v = r.u64();
    if (!r.ok() || r.remaining() != 0 || !timestamp) {
        if (error) *error = "PING 消息格式错误";
        return false;
    }
    *timestamp = v;
    return true;
}

}  // namespace p2p
