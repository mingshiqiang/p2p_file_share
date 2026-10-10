#include "p2p/LinkCore.h"

#include "protocol/SignalCodec.h"

#include <juice/juice.h>

#include <random>

namespace p2p {
namespace {

TransferId randomId() {
    std::random_device device;
    TransferId id;
    for (uint8_t& byte : id.bytes) byte = static_cast<uint8_t>(device());
    return id;
}

const char* kNoCandidate =
    "蓝牙连接成功，但 P2P 直连失败。当前没有可用的网络候选地址。请尝试让两台设备连接同一 Wi-Fi 网络。";

}  // namespace

LinkCore::LinkCore(Role role, std::string deviceId, std::string deviceName, Callbacks callbacks)
    : role_(role),
      deviceId_(std::move(deviceId)),
      deviceName_(std::move(deviceName)),
      cb_(std::move(callbacks)),
      engine_({
          [this](const uint8_t* data, size_t len) { return sendDatagram(data, len); },
          [this](const std::string& name, uint64_t size) {
              if (cb_.onOffer) cb_.onOffer(name, size);
          },
          [this](bool accepted, const std::string& reason) {
              if (cb_.onPeerDecision) cb_.onPeerDecision(accepted, reason);
          },
          [this](bool sending, const std::string& name, uint64_t done, uint64_t total) {
              if (cb_.onProgress) cb_.onProgress(sending, name, done, total);
          },
          [this](bool sending, bool ok, const std::string& message) {
              if (cb_.onFinished) cb_.onFinished(sending, ok, message);
              if (sending) pumpFiles();
          },
          [this]() {
              pingAnswered_ = true;
              log("P2P 通道已验证：收到 PONG");
          },
          [this](const std::string& line) { log(line); },
      }) {}

void LinkCore::setPoster(PostFn poster) { poster_ = std::move(poster); }

void LinkCore::post(std::function<void()> fn) {
    if (poster_) poster_(std::move(fn));
    else fn();
}

void LinkCore::log(const std::string& line) {
    if (cb_.onLog) cb_.onLog(line);
}

void LinkCore::setState(LinkState state, const std::string& detail) {
    state_ = state;
    if (cb_.onState) cb_.onState(static_cast<int>(state), detail);
}

void LinkCore::setAttPayload(size_t bytes) { fragmenter_.setAttPayload(bytes); }

void LinkCore::sendSignal(SignalType type, const std::vector<uint8_t>& payload) {
    const auto packets = fragmenter_.split(type, fragmenter_.nextMsgId(), payload.data(), payload.size());
    if (packets.empty() && !payload.empty()) {
        log("蓝牙信令分片失败");
        return;
    }
    for (const auto& packet : packets) {
        if (cb_.sendBle) cb_.sendBle(packet);
    }
}

void LinkCore::onTransportUp(uint64_t nowMs) {
    nowMs_ = nowMs;
    transportUp_ = true;
    helloSent_ = true;
    helloDeadlineMs_ = nowMs + 8000;
    setState(LinkState::BleConnected, "GATT 已连接，正在交换应用握手。这还不是 P2P 连接。");
    HelloBody hello;
    hello.version = kProtocolVersion;
    hello.role = role_;
    hello.deviceId = deviceId_;
    hello.deviceName = deviceName_;
    std::vector<uint8_t> payload;
    std::string error;
    if (!encodeHello(hello, &payload, &error)) {
        log(error);
        return;
    }
    sendSignal(SignalType::Hello, payload);
    log("已发送 HELLO");
}

void LinkCore::onTransportDown(const std::string& reason) {
    const bool wasSend = engine_.isSending();
    const bool wasRecv = engine_.isReceiving();
    engine_.reset();
    juice_.destroy();
    reassembler_ = BleReassembler();
    transportUp_ = false;
    helloSent_ = false;
    peerKnown_ = false;
    iceStarted_ = false;
    remoteSdpSet_ = false;
    localGatheringDone_ = false;
    remoteGatheringDone_ = false;
    p2pReady_ = false;
    pingAnswered_ = false;
    localCandidates_ = 0;
    remoteCandidates_ = 0;
    pingAttempts_ = 0;
    remoteDonePending_ = false;
    peerId_.clear();
    peerName_.clear();
    remoteSdp_.clear();
    pendingRemoteCandidates_.clear();
    fileQueue_.clear();
    if (wasSend && cb_.onFinished) cb_.onFinished(true, false, "连接已断开，发送已停止");
    if (wasRecv && cb_.onFinished) cb_.onFinished(false, false, "连接已断开，接收已停止");
    setState(LinkState::Disconnected, reason.empty() ? "连接已断开" : reason);
}

void LinkCore::onBlePacket(const uint8_t* data, size_t len, uint64_t nowMs) {
    nowMs_ = nowMs;
    const auto message = reassembler_.push(data, len, nowMs);
    if (!message.error.empty()) log(message.error);
    if (!message.ok) return;
    handleSignal(message.type, message.payload, nowMs);
}

void LinkCore::notePeer(const std::string& id, const std::string& name, uint8_t version) {
    if (version != kProtocolVersion) {
        log("对端协议版本不匹配");
        ErrorBody body;
        body.code = 1;
        body.message = "协议版本不匹配";
        std::vector<uint8_t> payload;
        encodeError(body, &payload, nullptr);
        sendSignal(SignalType::Error, payload);
        return;
    }
    peerId_ = id;
    peerName_ = name.empty() ? "未命名设备" : name;
    peerKnown_ = true;
    log("已确认对端应用：" + peerName_ + " / " + peerId_);
    if (cb_.onPeer) cb_.onPeer(peerName_, peerId_);
}

void LinkCore::maybeStartIce(uint64_t nowMs) {
    if (!peerKnown_ || iceStarted_ || !transportUp_) return;
    startIce(nowMs);
}

void LinkCore::startIce(uint64_t nowMs) {
    iceStarted_ = true;
    iceStartMs_ = nowMs;
    std::string error;
    JuiceSession::Handlers handlers;
    handlers.onState = [this](int state, std::string name) {
        post([this, state, name = std::move(name)] { onJuiceState(state, name); });
    };
    handlers.onCandidate = [this](std::string candidate) {
        post([this, candidate = std::move(candidate)] { onLocalCandidate(candidate); });
    };
    handlers.onGatheringDone = [this] { post([this] { onLocalGatheringDone(); }); };
    handlers.onRecv = [this](std::vector<uint8_t> data) {
        post([this, data = std::move(data)] { onJuiceRecv(std::move(data)); });
    };
    handlers.onLog = [this](std::string line) { post([this, line = std::move(line)] { log("libjuice: " + line); }); };
    if (!juice_.create(std::move(handlers), &error)) {
        failIce(error.empty() ? "创建 P2P agent 失败" : error);
        return;
    }
    std::string sdp;
    if (!juice_.localDescription(&sdp, &error)) {
        failIce(error.empty() ? "读取本地 SDP 失败" : error);
        return;
    }
    std::vector<uint8_t> payload;
    if (!encodeText(sdp, &payload, &error)) {
        failIce(error);
        return;
    }
    sendSignal(SignalType::Sdp, payload);
    setState(LinkState::IceGathering, "蓝牙已连接。正在通过蓝牙交换 P2P 描述，尚未建立 UDP 连接。");
    log("已发送本地 SDP");
    if (remoteSdpSet_) {
        if (!juice_.setRemoteDescription(remoteSdp_, &error) || !juice_.gatherCandidates(&error)) {
            failIce(error.empty() ? "设置远端 SDP 后收集候选失败" : error);
            return;
        }
        for (const auto& candidate : pendingRemoteCandidates_) juice_.addRemoteCandidate(candidate, nullptr);
        pendingRemoteCandidates_.clear();
        if (remoteDonePending_) {
            juice_.setRemoteGatheringDone(nullptr);
            remoteDonePending_ = false;
            remoteGatheringDone_ = true;
        }
        setState(LinkState::IceConnecting, "正在建立 P2P 连接");
    }
}

void LinkCore::onLocalCandidate(const std::string& candidate) {
    if (candidate.empty()) return;
    ++localCandidates_;
    std::vector<uint8_t> payload;
    std::string error;
    if (!encodeText(candidate, &payload, &error)) {
        log(error);
        return;
    }
    sendSignal(SignalType::Candidate, payload);
    log("已发送本地候选地址，累计 " + std::to_string(localCandidates_));
}

void LinkCore::onLocalGatheringDone() {
    localGatheringDone_ = true;
    sendSignal(SignalType::GatheringDone, {});
    log("本地候选收集完成，数量 " + std::to_string(localCandidates_));
    if (localCandidates_ == 0) failIce(kNoCandidate);
}

void LinkCore::onJuiceRecv(std::vector<uint8_t> data) {
    engine_.onDatagram(data.data(), data.size(), nowMs_);
}

void LinkCore::onJuiceState(int juiceState, const std::string& name) {
    log(std::string("libjuice 状态: ") + name);
    if (juiceState == JUICE_STATE_GATHERING) {
        setState(LinkState::IceGathering, "正在收集本机网络候选地址");
    } else if (juiceState == JUICE_STATE_CONNECTING) {
        setState(LinkState::IceConnecting, "正在尝试 UDP 直连");
    } else if (juiceState == JUICE_STATE_CONNECTED || juiceState == JUICE_STATE_COMPLETED) {
        if (!p2pReady_) {
            p2pReady_ = true;
            const std::string summary = juice_.selectedSummary();
            if (!summary.empty()) log(summary);
            setState(LinkState::P2PConnected, summary.empty() ? "P2P 已连接，可以传输文件" : summary);
            pingAttempts_ = 1;
            lastPingMs_ = nowMs_;
            engine_.sendPing(nowMs_);
            pumpFiles();
        }
    } else if (juiceState == JUICE_STATE_FAILED) {
        if (localCandidates_ == 0 && remoteCandidates_ == 0) failIce(kNoCandidate);
        else {
            failIce(std::string("蓝牙连接成功，但 P2P 直连失败。已交换网络候选地址，但 UDP 直连没有成功。"
                                "请尝试让两台设备连接同一 Wi-Fi 网络。libjuice 状态: ") +
                    name);
        }
    }
}

void LinkCore::failIce(const std::string& message) {
    p2pReady_ = false;
    log(message);
    setState(LinkState::IceFailed, message);
}

void LinkCore::handleSignal(SignalType type, const std::vector<uint8_t>& payload, uint64_t nowMs) {
    std::string error;
    switch (type) {
    case SignalType::Hello:
    case SignalType::HelloAck: {
        HelloBody hello;
        if (!decodeHello(payload.data(), payload.size(), &hello, &error)) {
            log(error);
            return;
        }
        notePeer(hello.deviceId, hello.deviceName, hello.version);
        if (type == SignalType::Hello) {
            HelloBody ack;
            ack.version = kProtocolVersion;
            ack.role = role_;
            ack.deviceId = deviceId_;
            ack.deviceName = deviceName_;
            std::vector<uint8_t> body;
            if (encodeHello(ack, &body, &error)) sendSignal(SignalType::HelloAck, body);
        }
        setState(LinkState::BleConnected, "蓝牙已连接：" + peerName_ + "（" + peerId_ + "）。P2P 尚未连接。");
        maybeStartIce(nowMs);
        break;
    }
    case SignalType::Sdp: {
        std::string sdp;
        if (!decodeText(payload.data(), payload.size(), &sdp, &error)) {
            log(error);
            return;
        }
        remoteSdp_ = sdp;
        remoteSdpSet_ = true;
        log("已收到远端 SDP");
        if (!iceStarted_) return;
        if (!juice_.setRemoteDescription(sdp, &error) || !juice_.gatherCandidates(&error)) {
            failIce(error.empty() ? "处理远端 SDP 失败" : error);
            return;
        }
        for (const auto& candidate : pendingRemoteCandidates_) juice_.addRemoteCandidate(candidate, nullptr);
        pendingRemoteCandidates_.clear();
        if (remoteDonePending_) {
            juice_.setRemoteGatheringDone(nullptr);
            remoteDonePending_ = false;
            remoteGatheringDone_ = true;
        }
        setState(LinkState::IceConnecting, "正在建立 P2P 连接");
        break;
    }
    case SignalType::Candidate: {
        std::string candidate;
        if (!decodeText(payload.data(), payload.size(), &candidate, &error)) {
            log(error);
            return;
        }
        ++remoteCandidates_;
        if (!remoteSdpSet_ || !juice_.valid()) pendingRemoteCandidates_.push_back(candidate);
        else if (!juice_.addRemoteCandidate(candidate, &error)) log(error);
        log("已收到远端候选地址，累计 " + std::to_string(remoteCandidates_));
        break;
    }
    case SignalType::GatheringDone:
        if (!remoteSdpSet_ || !juice_.valid()) remoteDonePending_ = true;
        else {
            juice_.setRemoteGatheringDone(nullptr);
            remoteGatheringDone_ = true;
        }
        log("远端候选收集完成");
        break;
    case SignalType::Bye:
        onTransportDown("对端已断开");
        break;
    case SignalType::Error: {
        ErrorBody body;
        if (decodeError(payload.data(), payload.size(), &body, &error)) log("对端错误: " + body.message);
        else log(error);
        break;
    }
    }
}

int LinkCore::sendDatagram(const uint8_t* data, size_t len) {
    if (!p2pReady_ || !juice_.valid()) return FileTransferEngine::kSendFail;
    const int rc = juice_.send(data, len);
    if (rc == JUICE_ERR_SUCCESS) return FileTransferEngine::kSendOk;
    if (rc == JUICE_ERR_AGAIN) return FileTransferEngine::kSendAgain;
    log(std::string("juice_send 失败，代码 ") + std::to_string(rc));
    return FileTransferEngine::kSendFail;
}

void LinkCore::pumpFiles() {
    if (!p2pReady_ || engine_.isSending() || fileQueue_.empty()) return;
    PendingFile next = std::move(fileQueue_.front());
    fileQueue_.pop_front();
    std::string error;
    if (!engine_.offerSend(randomId(), next.name, next.size, std::move(next.read), &error)) {
        log(error.empty() ? "无法开始发送文件" : error);
        if (cb_.onFinished) cb_.onFinished(true, false, error.empty() ? "无法开始发送文件" : error);
        pumpFiles();
    }
}

bool LinkCore::queueFile(const std::string& safeName, uint64_t size, ReadFn read, std::string* error) {
    if (!read) {
        if (error) *error = "没有可读的文件";
        return false;
    }
    fileQueue_.push_back(PendingFile{safeName, size, std::move(read)});
    if (!p2pReady_) {
        log("文件已加入队列。P2P 尚未连接，连接成功后才会发送：" + safeName);
        return true;
    }
    pumpFiles();
    return true;
}

void LinkCore::acceptFile(WriteFn write, std::function<void()> discard) { engine_.acceptReceive(std::move(write), std::move(discard)); }
void LinkCore::rejectFile(const std::string& reason) { engine_.rejectReceive(reason); }
void LinkCore::cancelSend(const std::string& reason) { engine_.cancelSend(reason); }
void LinkCore::cancelReceive(const std::string& reason) { engine_.cancelReceive(reason); }

void LinkCore::tick(uint64_t nowMs) {
    nowMs_ = nowMs;
    std::string error;
    if (reassembler_.expire(nowMs, &error)) log(error);
    engine_.tick(nowMs);
    if (transportUp_ && !peerKnown_ && helloDeadlineMs_ != 0 && nowMs > helloDeadlineMs_) {
        helloDeadlineMs_ = 0;
        log("蓝牙已连接，但未收到对端应用握手");
    }
    if (iceStarted_ && !p2pReady_ && state_ != LinkState::IceFailed) {
        if (localGatheringDone_ && remoteGatheringDone_ && gatheringReadyMs_ == 0) gatheringReadyMs_ = nowMs;
        const bool noCandidates = localGatheringDone_ && localCandidates_ == 0;
        if (noCandidates) {
            failIce(kNoCandidate);
        } else if (gatheringReadyMs_ != 0 && nowMs > gatheringReadyMs_ + 15000) {
            failIce("蓝牙连接成功，但 P2P 直连失败。候选地址已经交换，但在超时时间内没有建立 UDP 直连。"
                    "请尝试让两台设备连接同一 Wi-Fi 网络。");
        } else if (iceStartMs_ != 0 && nowMs > iceStartMs_ + 25000) {
            if (localCandidates_ == 0 && remoteCandidates_ == 0) failIce(kNoCandidate);
            else {
                failIce("蓝牙连接成功，但 P2P 直连失败。请尝试让两台设备连接同一 Wi-Fi 网络。");
            }
        }
    }
    if (p2pReady_ && !pingAnswered_ && pingAttempts_ < 3 && nowMs > lastPingMs_ + 2000) {
        ++pingAttempts_;
        lastPingMs_ = nowMs;
        engine_.sendPing(nowMs);
    }
}

}  // namespace p2p
