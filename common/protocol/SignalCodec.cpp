#include "protocol/SignalCodec.h"

#include "protocol/Buffer.h"

namespace p2p {
namespace {

constexpr size_t kMaxText = 4096;

bool tooLong(const std::string& text, std::string* error) {
    if (text.size() > kMaxText) {
        if (error) *error = "信令文本过长";
        return true;
    }
    return false;
}

}  // namespace

bool encodeHello(const HelloBody& body, std::vector<uint8_t>* out, std::string* error) {
    if (!out) return false;
    if (tooLong(body.deviceId, error) || tooLong(body.deviceName, error)) return false;
    out->clear();
    ByteWriter w(*out);
    w.u8(body.version);
    w.u8(static_cast<uint8_t>(body.role));
    w.str(body.deviceId);
    w.str(body.deviceName);
    return true;
}

bool decodeHello(const uint8_t* data, size_t len, HelloBody* out, std::string* error) {
    if (!out) return false;
    ByteReader r(data, len);
    out->version = r.u8();
    out->role = static_cast<Role>(r.u8());
    out->deviceId = r.str(kMaxText);
    out->deviceName = r.str(kMaxText);
    if (!r.ok() || r.remaining() != 0) {
        if (error) *error = "握手消息格式错误";
        return false;
    }
    return true;
}

bool encodeText(const std::string& text, std::vector<uint8_t>* out, std::string* error) {
    if (!out || tooLong(text, error)) return false;
    out->clear();
    ByteWriter w(*out);
    w.str(text);
    return true;
}

bool decodeText(const uint8_t* data, size_t len, std::string* out, std::string* error) {
    if (!out) return false;
    ByteReader r(data, len);
    *out = r.str(kMaxText);
    if (!r.ok() || r.remaining() != 0) {
        if (error) *error = "文本信令格式错误";
        return false;
    }
    return true;
}

bool encodeError(const ErrorBody& body, std::vector<uint8_t>* out, std::string* error) {
    if (!out || tooLong(body.message, error)) return false;
    out->clear();
    ByteWriter w(*out);
    w.u16(body.code);
    w.str(body.message);
    return true;
}

bool decodeError(const uint8_t* data, size_t len, ErrorBody* out, std::string* error) {
    if (!out) return false;
    ByteReader r(data, len);
    out->code = r.u16();
    out->message = r.str(kMaxText);
    if (!r.ok() || r.remaining() != 0) {
        if (error) *error = "错误信令格式错误";
        return false;
    }
    return true;
}

}  // namespace p2p
