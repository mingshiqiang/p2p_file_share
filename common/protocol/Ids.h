#pragma once

#include <cstddef>
#include <cstdint>

// 两端必须使用同一套 UUID 与协议版本，禁止各自生成。
namespace p2p {

constexpr uint8_t kProtocolVersion = 1;

// 逻辑 UUID：6b8a1f40-2c7d-4e15-9a33-0d5e7c1b4a86
// 蓝牙广播里的 128-bit UUID 使用小端线序。
constexpr uint8_t kServiceUuidWire[16] = {
    0x40, 0x1f, 0x8a, 0x6b, 0x7d, 0x2c, 0x15, 0x4e,
    0x9a, 0x33, 0x0d, 0x5e, 0x7c, 0x1b, 0x4a, 0x86};

constexpr char kServiceUuid[] = "6b8a1f40-2c7d-4e15-9a33-0d5e7c1b4a86";
constexpr char kRxUuid[] = "6b8a1f41-2c7d-4e15-9a33-0d5e7c1b4a86";
constexpr char kTxUuid[] = "6b8a1f42-2c7d-4e15-9a33-0d5e7c1b4a86";
constexpr char kCccdUuid[] = "00002902-0000-1000-8000-00805f9b34fb";

constexpr uint8_t kRxUuidWire[16] = {
    0x41, 0x1f, 0x8a, 0x6b, 0x7d, 0x2c, 0x15, 0x4e,
    0x9a, 0x33, 0x0d, 0x5e, 0x7c, 0x1b, 0x4a, 0x86};
constexpr uint8_t kTxUuidWire[16] = {
    0x42, 0x1f, 0x8a, 0x6b, 0x7d, 0x2c, 0x15, 0x4e,
    0x9a, 0x33, 0x0d, 0x5e, 0x7c, 0x1b, 0x4a, 0x86};

enum class SignalType : uint8_t {
    Hello = 1,
    HelloAck = 2,
    Sdp = 3,
    Candidate = 4,
    GatheringDone = 5,
    Bye = 6,
    Error = 7,
};

enum class Role : uint8_t {
    AndroidPeripheral = 1,
    WindowsCentral = 2,
};

enum class LinkState : int {
    Idle = 0,
    BleScanning,
    BleConnecting,
    BleConnected,
    IceGathering,
    IceConnecting,
    P2PConnected,
    IceFailed,
    Disconnecting,
    Disconnected,
};

enum class FileMsgType : uint8_t {
    Ping = 1,
    Pong = 2,
    FileOffer = 3,
    FileAccept = 4,
    FileReject = 5,
    FileStart = 6,
    FileChunk = 7,
    FileAck = 8,
    FileEnd = 9,
    FileFinish = 10,
    FileCancel = 11,
    FileError = 12,
};

enum class FileErrorCode : uint16_t {
    Generic = 1,
    Rejected = 2,
    Cancelled = 3,
    HashMismatch = 4,
    SizeMismatch = 5,
    IoError = 6,
    Timeout = 7,
    Busy = 8,
    BadFrame = 9,
    NotAllowed = 10,
};

constexpr size_t kBleHeaderSize = 12;
constexpr size_t kDefaultAttPayload = 20;
constexpr size_t kMaxSignalBytes = 48 * 1024;
constexpr uint32_t kReassemblyTimeoutMs = 8000;

constexpr size_t kFileHeaderSize = 24;
constexpr size_t kMaxDatagram = 1200;
constexpr size_t kChunkPayload = 1000;
constexpr uint32_t kSendWindow = 16;
constexpr int kMaxRetries = 8;
constexpr uint64_t kRetransmitMs = 400;
constexpr uint64_t kOfferTimeoutMs = 60000;
constexpr uint64_t kFinishTimeoutMs = 15000;

constexpr char kFileMagic[4] = {'A', 'F', 'S', '1'};

inline const char* linkStateText(LinkState state) {
    switch (state) {
    case LinkState::Idle: return "空闲";
    case LinkState::BleScanning: return "正在扫描蓝牙";
    case LinkState::BleConnecting: return "正在连接蓝牙";
    case LinkState::BleConnected: return "蓝牙已连接";
    case LinkState::IceGathering: return "正在收集网络候选地址";
    case LinkState::IceConnecting: return "正在建立 P2P 连接";
    case LinkState::P2PConnected: return "P2P 已连接";
    case LinkState::IceFailed: return "P2P 直连失败";
    case LinkState::Disconnecting: return "正在断开";
    case LinkState::Disconnected: return "已断开";
    }
    return "未知状态";
}

}  // namespace p2p
