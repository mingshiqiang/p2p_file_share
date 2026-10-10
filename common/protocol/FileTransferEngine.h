#pragma once

#include "protocol/FileFrame.h"
#include "protocol/Sha256.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace p2p {

// libjuice 只提供 UDP。这个类在其上实现单文件、双向独立的可靠传输。
class FileTransferEngine {
public:
    static constexpr int kSendOk = 0;
    static constexpr int kSendAgain = 5;
    static constexpr int kSendFail = -1;

    using ReadFn = std::function<bool(uint64_t offset, uint8_t* dst, size_t len, size_t* got, std::string* error)>;
    using WriteFn = std::function<bool(const uint8_t* data, size_t len, std::string* error)>;
    using DiscardFn = std::function<void()>;

    struct Callbacks {
        std::function<int(const uint8_t* data, size_t len)> sendDatagram;
        std::function<void(const std::string& fileName, uint64_t size)> onOffer;
        std::function<void(bool accepted, const std::string& reason)> onPeerDecision;
        std::function<void(bool sending, const std::string& fileName, uint64_t done, uint64_t total)> onProgress;
        std::function<void(bool sending, bool ok, const std::string& message)> onFinished;
        std::function<void()> onPong;
        std::function<void(const std::string& line)> onLog;
    };

    explicit FileTransferEngine(Callbacks callbacks);

    bool offerSend(const TransferId& id, const std::string& safeName, uint64_t size, ReadFn read, std::string* error);
    void acceptReceive(WriteFn write, DiscardFn discard);
    void rejectReceive(const std::string& reason);
    void cancelSend(const std::string& reason);
    void cancelReceive(const std::string& reason);
    void reset();

    void onDatagram(const uint8_t* data, size_t len, uint64_t nowMs);
    void tick(uint64_t nowMs);
    void sendPing(uint64_t nowMs);

    bool isSending() const;
    bool isReceiving() const;
    bool waitingForUser() const;

private:
    enum class SendState { Idle, WaitAccept, Sending, WaitFinish };
    enum class RecvState { Idle, WaitUser, Receiving };

    struct Inflight {
        uint32_t seq = 0;
        uint64_t offset = 0;
        std::vector<uint8_t> packet;
        uint64_t lastSendMs = 0;
        int retries = 0;
    };

    void log(const std::string& line);
    bool enqueue(std::vector<uint8_t> packet, bool front);
    void pump();
    bool queueFrame(const FileFrame& frame, bool front);
    void queueError(const TransferId& id, FileErrorCode code, const std::string& message);
    void failSend(const std::string& message, bool notifyPeer);
    void failRecv(const std::string& message, bool notifyPeer, bool discard);
    void finishSend(bool ok, const std::string& message);
    void finishRecv(bool ok, const std::string& message, bool discard);
    void clearSend();
    void clearRecv();
    void fillWindow();
    void sendEnd();
    void sendAck();
    void emitProgress(bool sending, bool force);
    void handleOffer(const FileFrame& frame);
    void handleChunk(const FileFrame& frame);
    void writeChunk(uint32_t seq, uint64_t offset, const std::vector<uint8_t>& data);
    void handleEnd(const FileFrame& frame);

    Callbacks cb_;
    std::deque<std::vector<uint8_t>> outbox_;
    bool pumping_ = false;
    uint64_t nowMs_ = 0;

    SendState sendState_ = SendState::Idle;
    TransferId sendId_{};
    std::string sendName_;
    uint64_t sendSize_ = 0;
    uint64_t sendOffset_ = 0;
    uint32_t sendNextSeq_ = 0;
    ReadFn read_;
    Sha256 sendHash_;
    std::map<uint32_t, Inflight> inflight_;
    uint64_t offerMs_ = 0;
    uint64_t endMs_ = 0;
    uint64_t sendAckedBytes_ = 0;
    uint64_t sendProgressMs_ = 0;

    RecvState recvState_ = RecvState::Idle;
    TransferId recvId_{};
    std::string recvName_;
    uint64_t recvSize_ = 0;
    uint32_t nextExpected_ = 0;
    uint64_t recvWritten_ = 0;
    WriteFn write_;
    DiscardFn discard_;
    Sha256 recvHash_;
    std::map<uint32_t, std::pair<uint64_t, std::vector<uint8_t>>> ahead_;
    uint64_t recvProgressMs_ = 0;
    bool recvFinished_ = false;
};

}  // namespace p2p
