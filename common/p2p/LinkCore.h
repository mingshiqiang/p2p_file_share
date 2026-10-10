#pragma once

#include "p2p/JuiceSession.h"
#include "protocol/BleFramer.h"
#include "protocol/FileTransferEngine.h"
#include "protocol/Ids.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

namespace p2p {

// 蓝牙信令、libjuice 和文件传输的共享状态机。调用方必须把所有入口放到同一个线程。
class LinkCore {
public:
    struct Callbacks {
        std::function<void(std::vector<uint8_t> packet)> sendBle;
        std::function<void(int state, std::string detail)> onState;
        std::function<void(std::string line)> onLog;
        std::function<void(std::string fileName, uint64_t size)> onOffer;
        std::function<void(bool sending, std::string fileName, uint64_t done, uint64_t total)> onProgress;
        std::function<void(bool sending, bool ok, std::string message)> onFinished;
        std::function<void(bool accepted, std::string reason)> onPeerDecision;
        std::function<void(std::string name, std::string id)> onPeer;
    };

    using ReadFn = FileTransferEngine::ReadFn;
    using WriteFn = FileTransferEngine::WriteFn;
    using PostFn = std::function<void(std::function<void()>)>;

    LinkCore(Role role, std::string deviceId, std::string deviceName, Callbacks callbacks);
    void setPoster(PostFn poster);

    void setAttPayload(size_t bytes);
    void onTransportUp(uint64_t nowMs);
    void onTransportDown(const std::string& reason);
    void onBlePacket(const uint8_t* data, size_t len, uint64_t nowMs);
    void tick(uint64_t nowMs);

    bool queueFile(const std::string& safeName, uint64_t size, ReadFn read, std::string* error);
    void acceptFile(WriteFn write, std::function<void()> discard);
    void rejectFile(const std::string& reason);
    void cancelSend(const std::string& reason);
    void cancelReceive(const std::string& reason);

    LinkState state() const { return state_; }
    bool p2pReady() const { return p2pReady_; }
    const std::string& peerName() const { return peerName_; }
    const std::string& peerId() const { return peerId_; }

private:
    struct PendingFile {
        std::string name;
        uint64_t size = 0;
        ReadFn read;
    };

    void log(const std::string& line);
    void setState(LinkState state, const std::string& detail);
    void sendSignal(SignalType type, const std::vector<uint8_t>& payload);
    void handleSignal(SignalType type, const std::vector<uint8_t>& payload, uint64_t nowMs);
    void notePeer(const std::string& id, const std::string& name, uint8_t version);
    void maybeStartIce(uint64_t nowMs);
    void startIce(uint64_t nowMs);
    void onJuiceState(int juiceState, const std::string& name);
    void onLocalCandidate(const std::string& candidate);
    void onLocalGatheringDone();
    void onJuiceRecv(std::vector<uint8_t> data);
    void failIce(const std::string& message);
    void pumpFiles();
    int sendDatagram(const uint8_t* data, size_t len);
    void post(std::function<void()> fn);

    Role role_;
    std::string deviceId_;
    std::string deviceName_;
    Callbacks cb_;
    PostFn poster_;
    BleFragmenter fragmenter_;
    BleReassembler reassembler_;
    FileTransferEngine engine_;
    JuiceSession juice_;

    LinkState state_ = LinkState::Idle;
    bool transportUp_ = false;
    bool helloSent_ = false;
    bool peerKnown_ = false;
    bool iceStarted_ = false;
    bool remoteSdpSet_ = false;
    bool localGatheringDone_ = false;
    bool remoteGatheringDone_ = false;
    bool p2pReady_ = false;
    bool pingAnswered_ = false;
    int localCandidates_ = 0;
    int remoteCandidates_ = 0;
    int pingAttempts_ = 0;
    uint64_t nowMs_ = 0;
    uint64_t iceStartMs_ = 0;
    uint64_t gatheringReadyMs_ = 0;
    uint64_t lastPingMs_ = 0;
    uint64_t helloDeadlineMs_ = 0;
    std::string peerId_;
    std::string peerName_;
    std::string remoteSdp_;
    std::vector<std::string> pendingRemoteCandidates_;
    std::deque<PendingFile> fileQueue_;
    bool remoteDonePending_ = false;
};

}  // namespace p2p
