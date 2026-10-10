#pragma once

#include "protocol/Ids.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace p2p {

struct TransferId {
    uint8_t bytes[16]{};
    bool operator==(const TransferId& other) const;
    bool operator!=(const TransferId& other) const { return !(*this == other); }
    bool empty() const;
    std::string hex() const;
};

struct FileFrame {
    FileMsgType type = FileMsgType::Ping;
    uint8_t flags = 0;
    TransferId id{};
    std::vector<uint8_t> body;
};

bool encodeFileFrame(const FileFrame& frame, std::vector<uint8_t>* out, std::string* error);
bool decodeFileFrame(const uint8_t* data, size_t len, FileFrame* out, std::string* error);

bool encodeOffer(const std::string& fileName, uint64_t fileSize, std::vector<uint8_t>* body, std::string* error);
bool decodeOffer(const uint8_t* data, size_t len, std::string* fileName, uint64_t* fileSize, std::string* error);
bool encodeReject(uint16_t code, const std::string& message, std::vector<uint8_t>* body, std::string* error);
bool decodeReject(const uint8_t* data, size_t len, uint16_t* code, std::string* message, std::string* error);
void encodeChunk(uint32_t seq, uint64_t offset, const uint8_t* data, uint16_t len, std::vector<uint8_t>* body);
bool decodeChunk(const uint8_t* data, size_t len, uint32_t* seq, uint64_t* offset, std::vector<uint8_t>* payload,
                 std::string* error);
void encodeAck(uint32_t nextExpected, std::vector<uint8_t>* body);
bool decodeAck(const uint8_t* data, size_t len, uint32_t* nextExpected, std::string* error);
void encodeEnd(uint64_t fileSize, uint32_t totalChunks, const std::array<uint8_t, 32>& hash, std::vector<uint8_t>* body);
bool decodeEnd(const uint8_t* data, size_t len, uint64_t* fileSize, uint32_t* totalChunks, std::array<uint8_t, 32>* hash,
               std::string* error);
void encodeHash(const std::array<uint8_t, 32>& hash, std::vector<uint8_t>* body);
bool decodeHash(const uint8_t* data, size_t len, std::array<uint8_t, 32>* hash, std::string* error);
void encodeStart(uint64_t fileSize, uint32_t chunkSize, std::vector<uint8_t>* body);
bool decodeStart(const uint8_t* data, size_t len, uint64_t* fileSize, uint32_t* chunkSize, std::string* error);
void encodePing(uint64_t timestamp, std::vector<uint8_t>* body);
bool decodePing(const uint8_t* data, size_t len, uint64_t* timestamp, std::string* error);

}  // namespace p2p
