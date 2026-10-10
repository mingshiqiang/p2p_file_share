#include "protocol/FileTransferEngine.h"

#include "protocol/FileName.h"

#include <cstring>

namespace p2p {
namespace {

std::string withName(const std::string& message, const std::string& name) {
    if (name.empty()) return message;
    return message + "（" + name + "）";
}

}  // namespace

FileTransferEngine::FileTransferEngine(Callbacks callbacks) : cb_(std::move(callbacks)) {}

bool FileTransferEngine::isSending() const { return sendState_ != SendState::Idle; }
bool FileTransferEngine::isReceiving() const { return recvState_ != RecvState::Idle; }
bool FileTransferEngine::waitingForUser() const { return recvState_ == RecvState::WaitUser; }

void FileTransferEngine::log(const std::string& line) {
    if (cb_.onLog) cb_.onLog(line);
}

bool FileTransferEngine::enqueue(std::vector<uint8_t> packet, bool front) {
    if (outbox_.size() >= 256) return false;
    if (front) outbox_.push_front(std::move(packet));
    else outbox_.push_back(std::move(packet));
    return true;
}

void FileTransferEngine::pump() {
    // 发送回调里可能会立刻处理对端响应并再次进入 pump。必须先把包取出，并忽略重入。
    if (pumping_) return;
    pumping_ = true;
    while (!outbox_.empty()) {
        if (!cb_.sendDatagram) {
            outbox_.clear();
            break;
        }
        std::vector<uint8_t> packet = std::move(outbox_.front());
        outbox_.pop_front();
        const int rc = cb_.sendDatagram(packet.data(), packet.size());
        if (rc == kSendAgain) {
            outbox_.push_front(std::move(packet));
            break;
        }
        if (rc != kSendOk) {
            outbox_.clear();
            if (isSending()) failSend("P2P 发送失败", false);
            else if (isReceiving()) failRecv("P2P 发送失败", false, true);
            break;
        }
    }
    pumping_ = false;
}

bool FileTransferEngine::queueFrame(const FileFrame& frame, bool front) {
    std::vector<uint8_t> packet;
    std::string error;
    if (!encodeFileFrame(frame, &packet, &error)) {
        log(error);
        return false;
    }
    if (!enqueue(std::move(packet), front)) {
        log("发送队列已满");
        return false;
    }
    return true;
}

void FileTransferEngine::queueError(const TransferId& id, FileErrorCode code, const std::string& message) {
    FileFrame frame;
    frame.type = FileMsgType::FileError;
    frame.id = id;
    encodeReject(static_cast<uint16_t>(code), message, &frame.body, nullptr);
    queueFrame(frame, true);
}

void FileTransferEngine::clearSend() {
    sendState_ = SendState::Idle;
    sendId_ = {};
    sendName_.clear();
    sendSize_ = 0;
    sendOffset_ = 0;
    sendNextSeq_ = 0;
    read_ = nullptr;
    sendHash_ = Sha256();
    inflight_.clear();
    offerMs_ = 0;
    endMs_ = 0;
    sendAckedBytes_ = 0;
}

void FileTransferEngine::clearRecv() {
    recvState_ = RecvState::Idle;
    recvId_ = {};
    recvName_.clear();
    recvSize_ = 0;
    nextExpected_ = 0;
    recvWritten_ = 0;
    write_ = nullptr;
    discard_ = nullptr;
    recvHash_ = Sha256();
    ahead_.clear();
    recvFinished_ = false;
}

void FileTransferEngine::finishSend(bool ok, const std::string& message) {
    const std::string name = sendName_;
    clearSend();
    if (cb_.onFinished) cb_.onFinished(true, ok, withName(message, name));
}

void FileTransferEngine::finishRecv(bool ok, const std::string& message, bool discard) {
    const std::string name = recvName_;
    DiscardFn discardFn = discard ? discard_ : DiscardFn{};
    clearRecv();
    if (discardFn) discardFn();
    if (cb_.onFinished) cb_.onFinished(false, ok, withName(message, name));
}

void FileTransferEngine::failSend(const std::string& message, bool notifyPeer) {
    if (sendState_ == SendState::Idle) return;
    const TransferId id = sendId_;
    if (notifyPeer) queueError(id, FileErrorCode::Generic, message);
    log("发送失败: " + message);
    finishSend(false, message);
    pump();
}

void FileTransferEngine::failRecv(const std::string& message, bool notifyPeer, bool discard) {
    if (recvState_ == RecvState::Idle) return;
    const TransferId id = recvId_;
    if (notifyPeer) queueError(id, FileErrorCode::IoError, message);
    log("接收失败: " + message);
    finishRecv(false, message, discard);
    pump();
}

void FileTransferEngine::emitProgress(bool sending, bool force) {
    const uint64_t done = sending ? sendAckedBytes_ : recvWritten_;
    const uint64_t total = sending ? sendSize_ : recvSize_;
    uint64_t& stamp = sending ? sendProgressMs_ : recvProgressMs_;
    if (!force && nowMs_ < stamp + 200 && done != total) return;
    stamp = nowMs_;
    if (cb_.onProgress) cb_.onProgress(sending, sending ? sendName_ : recvName_, done, total);
}

bool FileTransferEngine::offerSend(const TransferId& id, const std::string& safeName, uint64_t size, ReadFn read,
                                   std::string* error) {
    if (sendState_ != SendState::Idle) {
        if (error) *error = "当前已有正在发送的文件";
        return false;
    }
    if (!read) {
        if (error) *error = "没有可读的文件";
        return false;
    }
    sendState_ = SendState::WaitAccept;
    sendId_ = id;
    sendName_ = safeName;
    sendSize_ = size;
    read_ = std::move(read);
    sendHash_ = Sha256();
    offerMs_ = nowMs_;
    FileFrame frame;
    frame.type = FileMsgType::FileOffer;
    frame.id = id;
    if (!encodeOffer(safeName, size, &frame.body, error) || !queueFrame(frame, false)) {
        clearSend();
        if (error && error->empty()) *error = "无法发送文件请求";
        return false;
    }
    log("已发送文件请求: " + safeName);
    pump();
    emitProgress(true, true);
    return true;
}

void FileTransferEngine::acceptReceive(WriteFn write, DiscardFn discard) {
    if (recvState_ != RecvState::WaitUser || !write) return;
    write_ = std::move(write);
    discard_ = std::move(discard);
    recvState_ = RecvState::Receiving;
    recvHash_ = Sha256();
    FileFrame frame;
    frame.type = FileMsgType::FileAccept;
    frame.id = recvId_;
    queueFrame(frame, true);
    log("已接受文件: " + recvName_);
    pump();
}

void FileTransferEngine::rejectReceive(const std::string& reason) {
    if (recvState_ != RecvState::WaitUser && recvState_ != RecvState::Receiving) return;
    FileFrame frame;
    frame.type = FileMsgType::FileReject;
    frame.id = recvId_;
    encodeReject(static_cast<uint16_t>(FileErrorCode::Rejected), reason, &frame.body, nullptr);
    queueFrame(frame, true);
    log("已拒绝文件: " + reason);
    finishRecv(false, reason.empty() ? "已拒绝接收" : reason, false);
    pump();
}

void FileTransferEngine::cancelSend(const std::string& reason) {
    if (sendState_ == SendState::Idle) return;
    FileFrame frame;
    frame.type = FileMsgType::FileCancel;
    frame.id = sendId_;
    encodeReject(static_cast<uint16_t>(FileErrorCode::Cancelled), reason, &frame.body, nullptr);
    queueFrame(frame, true);
    finishSend(false, reason.empty() ? "已取消发送" : reason);
    pump();
}

void FileTransferEngine::cancelReceive(const std::string& reason) {
    if (recvState_ == RecvState::Idle) return;
    FileFrame frame;
    frame.type = FileMsgType::FileCancel;
    frame.id = recvId_;
    encodeReject(static_cast<uint16_t>(FileErrorCode::Cancelled), reason, &frame.body, nullptr);
    queueFrame(frame, true);
    finishRecv(false, reason.empty() ? "已取消接收" : reason, true);
    pump();
}

void FileTransferEngine::reset() {
    outbox_.clear();
    clearSend();
    clearRecv();
}

void FileTransferEngine::sendPing(uint64_t nowMs) {
    nowMs_ = nowMs;
    FileFrame frame;
    frame.type = FileMsgType::Ping;
    encodePing(nowMs, &frame.body);
    queueFrame(frame, false);
    pump();
}

void FileTransferEngine::fillWindow() {
    if (sendState_ != SendState::Sending) return;
    while (inflight_.size() < kSendWindow && sendOffset_ < sendSize_) {
        const uint64_t remain = sendSize_ - sendOffset_;
        const size_t ask = static_cast<size_t>(remain > kChunkPayload ? static_cast<uint64_t>(kChunkPayload) : remain);
        std::vector<uint8_t> data(ask);
        size_t got = 0;
        std::string error;
        if (!read_(sendOffset_, data.data(), ask, &got, &error) || got == 0 || got > ask) {
            failSend(error.empty() ? "读取文件失败" : error, true);
            return;
        }
        data.resize(got);
        sendHash_.update(data.data(), data.size());
        FileFrame frame;
        frame.type = FileMsgType::FileChunk;
        frame.id = sendId_;
        encodeChunk(sendNextSeq_, sendOffset_, data.data(), static_cast<uint16_t>(data.size()), &frame.body);
        std::vector<uint8_t> packet;
        if (!encodeFileFrame(frame, &packet, &error)) {
            failSend(error, true);
            return;
        }
        Inflight item;
        item.seq = sendNextSeq_;
        item.offset = sendOffset_;
        item.packet = packet;
        item.lastSendMs = nowMs_;
        if (!enqueue(packet, false)) {
            failSend("发送队列已满", true);
            return;
        }
        inflight_[sendNextSeq_] = std::move(item);
        sendOffset_ += got;
        ++sendNextSeq_;
    }
    if (sendOffset_ == sendSize_ && inflight_.empty()) sendEnd();
    else pump();
}

void FileTransferEngine::sendEnd() {
    if (sendState_ != SendState::Sending) return;
    FileFrame frame;
    frame.type = FileMsgType::FileEnd;
    frame.id = sendId_;
    encodeEnd(sendSize_, sendNextSeq_, sendHash_.digest(), &frame.body);
    if (!queueFrame(frame, false)) {
        failSend("无法发送结束消息", false);
        return;
    }
    sendState_ = SendState::WaitFinish;
    endMs_ = nowMs_;
    sendAckedBytes_ = sendSize_;
    emitProgress(true, true);
    pump();
}

void FileTransferEngine::sendAck() {
    FileFrame frame;
    frame.type = FileMsgType::FileAck;
    frame.id = recvId_;
    encodeAck(nextExpected_, &frame.body);
    queueFrame(frame, true);
}

void FileTransferEngine::writeChunk(uint32_t seq, uint64_t offset, const std::vector<uint8_t>& data) {
    if (offset != recvWritten_) {
        failRecv("分块偏移与已写入长度不一致", true, true);
        return;
    }
    std::string error;
    if (!data.empty() && !write_(data.data(), data.size(), &error)) {
        failRecv(error.empty() ? "写入文件失败" : error, true, true);
        return;
    }
    if (!data.empty()) recvHash_.update(data.data(), data.size());
    recvWritten_ += data.size();
    ++nextExpected_;
    (void)seq;
    emitProgress(false, false);
}

void FileTransferEngine::handleChunk(const FileFrame& frame) {
    if (recvState_ != RecvState::Receiving || !(frame.id == recvId_)) return;
    uint32_t seq = 0;
    uint64_t offset = 0;
    std::vector<uint8_t> data;
    std::string error;
    if (!decodeChunk(frame.body.data(), frame.body.size(), &seq, &offset, &data, &error)) {
        failRecv(error, true, true);
        return;
    }
    if (seq < nextExpected_) {
        sendAck();
        pump();
        return;
    }
    if (seq != nextExpected_) {
        if (ahead_.size() >= 64) {
            failRecv("乱序分片过多", true, true);
            return;
        }
        ahead_[seq] = std::make_pair(offset, std::move(data));
        sendAck();
        pump();
        return;
    }
    writeChunk(seq, offset, data);
    if (recvState_ != RecvState::Receiving) return;
    while (ahead_.count(nextExpected_)) {
        auto item = std::move(ahead_[nextExpected_]);
        ahead_.erase(nextExpected_);
        writeChunk(nextExpected_, item.first, item.second);
        if (recvState_ != RecvState::Receiving) return;
    }
    sendAck();
    pump();
}

void FileTransferEngine::handleEnd(const FileFrame& frame) {
    if (recvState_ != RecvState::Receiving || !(frame.id == recvId_)) return;
    uint64_t size = 0;
    uint32_t chunks = 0;
    std::array<uint8_t, 32> hash{};
    std::string error;
    if (!decodeEnd(frame.body.data(), frame.body.size(), &size, &chunks, &hash, &error)) {
        failRecv(error, true, true);
        return;
    }
    const auto local = recvHash_.digest();
    if (size != recvSize_ || size != recvWritten_ || chunks != nextExpected_ || !ahead_.empty() || local != hash) {
        const char* why = local != hash ? "SHA-256 校验失败" : "文件长度或分块数量不一致";
        FileFrame reply;
        reply.type = FileMsgType::FileError;
        reply.id = recvId_;
        encodeReject(static_cast<uint16_t>(local != hash ? FileErrorCode::HashMismatch : FileErrorCode::SizeMismatch),
                     why, &reply.body, nullptr);
        queueFrame(reply, true);
        finishRecv(false, why, true);
        pump();
        return;
    }
    FileFrame reply;
    reply.type = FileMsgType::FileFinish;
    reply.id = recvId_;
    encodeHash(local, &reply.body);
    queueFrame(reply, true);
    finishRecv(true, "文件接收完成", false);
    pump();
}

void FileTransferEngine::handleOffer(const FileFrame& frame) {
    std::string name;
    uint64_t size = 0;
    std::string error;
    if (!decodeOffer(frame.body.data(), frame.body.size(), &name, &size, &error)) {
        queueError(frame.id, FileErrorCode::BadFrame, error);
        pump();
        return;
    }
    std::string safe;
    if (!sanitizeFileName(name, &safe, &error)) {
        FileFrame reply;
        reply.type = FileMsgType::FileReject;
        reply.id = frame.id;
        encodeReject(static_cast<uint16_t>(FileErrorCode::BadFrame), error, &reply.body, nullptr);
        queueFrame(reply, true);
        pump();
        return;
    }
    if (recvState_ != RecvState::Idle) {
        FileFrame reply;
        reply.type = FileMsgType::FileError;
        reply.id = frame.id;
        encodeReject(static_cast<uint16_t>(FileErrorCode::Busy), "正在处理另一个文件", &reply.body, nullptr);
        queueFrame(reply, true);
        pump();
        return;
    }
    recvState_ = RecvState::WaitUser;
    recvId_ = frame.id;
    recvName_ = safe;
    recvSize_ = size;
    log("收到文件请求: " + safe);
    if (cb_.onOffer) cb_.onOffer(safe, size);
}

void FileTransferEngine::onDatagram(const uint8_t* data, size_t len, uint64_t nowMs) {
    nowMs_ = nowMs;
    FileFrame frame;
    std::string error;
    if (!decodeFileFrame(data, len, &frame, &error)) {
        log(error.empty() ? "丢弃无法解析的数据报" : error);
        return;
    }
    switch (frame.type) {
    case FileMsgType::Ping: {
        uint64_t stamp = 0;
        if (!decodePing(frame.body.data(), frame.body.size(), &stamp, &error)) return;
        FileFrame reply;
        reply.type = FileMsgType::Pong;
        reply.id = frame.id;
        encodePing(stamp, &reply.body);
        queueFrame(reply, true);
        pump();
        break;
    }
    case FileMsgType::Pong:
        if (cb_.onPong) cb_.onPong();
        break;
    case FileMsgType::FileOffer:
        handleOffer(frame);
        break;
    case FileMsgType::FileAccept:
        if (sendState_ == SendState::WaitAccept && frame.id == sendId_) {
            sendState_ = SendState::Sending;
            if (cb_.onPeerDecision) cb_.onPeerDecision(true, {});
            FileFrame start;
            start.type = FileMsgType::FileStart;
            start.id = sendId_;
            encodeStart(sendSize_, static_cast<uint32_t>(kChunkPayload), &start.body);
            queueFrame(start, false);
            fillWindow();
        }
        break;
    case FileMsgType::FileReject: {
        uint16_t code = 0;
        std::string message;
        if (!decodeReject(frame.body.data(), frame.body.size(), &code, &message, &error)) message = "对方拒绝了文件";
        if (message.empty()) message = "对方拒绝了文件";
        if (sendState_ != SendState::Idle && frame.id == sendId_) {
            if (cb_.onPeerDecision) cb_.onPeerDecision(false, message);
            finishSend(false, message);
        } else if (recvState_ != RecvState::Idle && frame.id == recvId_) {
            finishRecv(false, message, true);
        }
        break;
    }
    case FileMsgType::FileStart: {
        uint64_t size = 0;
        uint32_t chunk = 0;
        if (recvState_ == RecvState::Receiving && frame.id == recvId_ &&
            decodeStart(frame.body.data(), frame.body.size(), &size, &chunk, &error)) {
            if (size != recvSize_ || chunk == 0 || chunk > kChunkPayload) failRecv("开始消息中的文件参数无效", true, true);
        }
        break;
    }
    case FileMsgType::FileChunk:
        handleChunk(frame);
        break;
    case FileMsgType::FileAck: {
        uint32_t nextExpected = 0;
        if (sendState_ != SendState::Sending && sendState_ != SendState::WaitFinish) break;
        if (frame.id != sendId_ || !decodeAck(frame.body.data(), frame.body.size(), &nextExpected, &error)) break;
        if (nextExpected > sendNextSeq_) break;
        uint64_t acked = 0;
        for (auto it = inflight_.begin(); it != inflight_.end();) {
            if (it->first < nextExpected) {
                acked += it->second.packet.empty() ? 0 : 0;
                // 已确认字节按分块载荷累计，而不是按整个 UDP 包。
                it = inflight_.erase(it);
            } else {
                ++it;
            }
        }
        // 累计确认对应的文件偏移就是已发送且序号小于 nextExpected 的连续区间。
        // 发送端按序号从 0 连续发送，因此已确认字节可由剩余窗口反推。
        if (inflight_.empty()) sendAckedBytes_ = sendOffset_;
        else sendAckedBytes_ = inflight_.begin()->second.offset;
        (void)acked;
        emitProgress(true, false);
        if (sendState_ == SendState::Sending) fillWindow();
        break;
    }
    case FileMsgType::FileEnd:
        handleEnd(frame);
        break;
    case FileMsgType::FileFinish:
        if (sendState_ == SendState::WaitFinish && frame.id == sendId_) {
            std::array<uint8_t, 32> hash{};
            if (!decodeHash(frame.body.data(), frame.body.size(), &hash, &error)) {
                failSend("完成确认格式错误", false);
                break;
            }
            emitProgress(true, true);
            finishSend(true, "文件发送完成");
        }
        break;
    case FileMsgType::FileCancel: {
        uint16_t code = 0;
        std::string message;
        decodeReject(frame.body.data(), frame.body.size(), &code, &message, &error);
        if (message.empty()) message = "对端取消了传输";
        if (sendState_ != SendState::Idle && frame.id == sendId_) finishSend(false, message);
        else if (recvState_ != RecvState::Idle && frame.id == recvId_) finishRecv(false, message, true);
        break;
    }
    case FileMsgType::FileError: {
        uint16_t code = 0;
        std::string message;
        decodeReject(frame.body.data(), frame.body.size(), &code, &message, &error);
        if (message.empty()) message = "对端报告传输错误";
        if (sendState_ != SendState::Idle && frame.id == sendId_) finishSend(false, message);
        else if (recvState_ != RecvState::Idle && frame.id == recvId_) finishRecv(false, message, true);
        break;
    }
    }
}

void FileTransferEngine::tick(uint64_t nowMs) {
    nowMs_ = nowMs;
    if (sendState_ == SendState::WaitAccept && offerMs_ == 0) offerMs_ = nowMs_;
    if (sendState_ == SendState::WaitAccept && offerMs_ != 0 && nowMs_ > offerMs_ &&
        nowMs_ - offerMs_ > kOfferTimeoutMs) {
        failSend("等待对方接受文件超时", true);
        return;
    }
    if (sendState_ == SendState::WaitFinish && endMs_ != 0 && nowMs_ - endMs_ > kFinishTimeoutMs) {
        failSend("等待接收端完成确认超时", true);
        return;
    }
    if (sendState_ == SendState::Sending) {
        bool timedOut = false;
        for (auto& item : inflight_) {
            if (nowMs_ >= item.second.lastSendMs && nowMs_ - item.second.lastSendMs >= kRetransmitMs) {
                if (item.second.retries >= kMaxRetries) {
                    timedOut = true;
                    break;
                }
                ++item.second.retries;
                item.second.lastSendMs = nowMs_;
                if (!enqueue(item.second.packet, false)) {
                    failSend("重传队列已满", true);
                    return;
                }
            }
        }
        if (timedOut) {
            failSend("分块重传次数已用尽", true);
            return;
        }
    }
    pump();
}

}  // namespace p2p
