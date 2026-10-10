#pragma once

#include "protocol/Ids.h"

#include <cstdint>
#include <string>
#include <vector>

namespace p2p {

struct HelloBody {
    uint8_t version = kProtocolVersion;
    Role role = Role::AndroidPeripheral;
    std::string deviceId;
    std::string deviceName;
};

struct ErrorBody {
    uint16_t code = 0;
    std::string message;
};

bool encodeHello(const HelloBody& body, std::vector<uint8_t>* out, std::string* error);
bool decodeHello(const uint8_t* data, size_t len, HelloBody* out, std::string* error);
bool encodeText(const std::string& text, std::vector<uint8_t>* out, std::string* error);
bool decodeText(const uint8_t* data, size_t len, std::string* out, std::string* error);
bool encodeError(const ErrorBody& body, std::vector<uint8_t>* out, std::string* error);
bool decodeError(const uint8_t* data, size_t len, ErrorBody* out, std::string* error);

}  // namespace p2p
