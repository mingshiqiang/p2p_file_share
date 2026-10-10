#include "protocol/BleFramer.h"
#include "protocol/Crc16.h"
#include "protocol/FileFrame.h"
#include "protocol/FileName.h"
#include "protocol/FileTransferEngine.h"
#include "protocol/Sha256.h"
#include "protocol/SignalCodec.h"

#include <algorithm>
#include <cstring>
#include <deque>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

int g_failed = 0;

void expect(bool condition, const std::string& name) {
    if (condition) {
        std::cout << "通过 " << name << "\n";
    } else {
        std::cout << "失败 " << name << "\n";
        ++g_failed;
    }
}

void testCrc() {
    const uint8_t data[] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    expect(p2p::crc16(data, sizeof(data)) == 0x29B1, "CRC-16");
}

void testSha() {
    p2p::Sha256 empty;
    expect(p2p::Sha256::hex(empty.digest()) ==
               "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
           "SHA-256 空内容");
    p2p::Sha256 abc;
    const char* text = "abc";
    abc.update(reinterpret_cast<const uint8_t*>(text), 3);
    expect(p2p::Sha256::hex(abc.digest()) ==
               "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
           "SHA-256 abc");
}

void testFragment() {
    p2p::BleFragmenter fragmenter;
    fragmenter.setAttPayload(20);
    const std::string payload = "hello-ble-fragment-payload-需要分片";
    auto packets = fragmenter.split(p2p::SignalType::Hello, 7, reinterpret_cast<const uint8_t*>(payload.data()),
                                    payload.size());
    expect(packets.size() > 1, "分片数量");
    p2p::BleReassembler reassembler;
    p2p::BleReassembler::Completed done;
    for (size_t i = packets.size(); i-- > 0;) {
        auto again = packets[packets.size() - 1];
        reassembler.push(again.data(), again.size(), 1000);
        done = reassembler.push(packets[i].data(), packets[i].size(), 1000);
    }
    expect(done.ok, "乱序重组");
    expect(std::string(done.payload.begin(), done.payload.end()) == payload, "重组内容");
    auto duplicate = reassembler.push(packets[0].data(), packets[0].size(), 1000);
    expect(duplicate.duplicate, "重复消息");
    packets[0][11] ^= 0xff;
    p2p::BleReassembler bad;
    auto corrupt = bad.push(packets[0].data(), packets[0].size(), 1000);
    expect(!corrupt.ok && !corrupt.error.empty(), "校验失败");
}

void testNames() {
    std::string safe;
    std::string error;
    expect(p2p::sanitizeFileName("C:\\temp\\报告.pdf", &safe, &error) && safe == "报告.pdf", "中文文件名");
    expect(p2p::sanitizeFileName("../secret.txt", &safe, &error) && safe == "secret.txt", "只保留文件名");
    expect(!p2p::sanitizeFileName("..", &safe, &error), "拒绝目录穿越");
    expect(!p2p::sanitizeFileName("D:evil.txt", &safe, &error), "拒绝盘符");
}

struct Pipe {
    p2p::FileTransferEngine* other = nullptr;
    std::deque<std::vector<uint8_t>> queue;
    bool pumping = false;
    int sent = 0;
    void send(const uint8_t* data, size_t len) {
        ++sent;
        if (sent % 3 == 0) return;
        queue.push_back(std::vector<uint8_t>(data, data + len));
        if (sent % 5 == 0 && !queue.empty()) queue.push_back(queue.back());
        pump();
    }
    void pump() {
        if (pumping) return;
        pumping = true;
        while (!queue.empty()) {
            auto packet = std::move(queue.front());
            queue.pop_front();
            if (other) other->onDatagram(packet.data(), packet.size(), 1000);
        }
        pumping = false;
    }
};

void testTransfer() {
    Pipe left;
    Pipe right;
    bool offer = false;
    bool finished = false;
    std::string received;
    p2p::FileTransferEngine receiver({
        [&](const uint8_t* data, size_t len) {
            left.send(data, len);
            return p2p::FileTransferEngine::kSendOk;
        },
        [&](const std::string& name, uint64_t size) {
            offer = name == "中文.txt" && size == std::string("你好A").size();
            receiver.acceptReceive([&](const uint8_t* data, size_t len, std::string*) {
                received.append(reinterpret_cast<const char*>(data), len);
                return true;
            }, [] {});
        },
        [](bool, const std::string&) {},
        [](bool, const std::string&, uint64_t, uint64_t) {},
        [&](bool sending, bool ok, const std::string& message) {
            if (!sending && ok && message.find("文件接收完成") != std::string::npos) finished = true;
        },
        [] {},
        [](const std::string&) {},
    });
    p2p::FileTransferEngine sender({
        [&](const uint8_t* data, size_t len) {
            right.send(data, len);
            return p2p::FileTransferEngine::kSendOk;
        },
        [](const std::string&, uint64_t) {},
        [](bool, const std::string&) {},
        [](bool, const std::string&, uint64_t, uint64_t) {},
        [](bool, bool, const std::string&) {},
        [] {},
        [](const std::string&) {},
    });
    left.other = &sender;
    right.other = &receiver;
    const std::string body = "你好A";
    p2p::TransferId id{};
    id.bytes[0] = 9;
    sender.tick(1000);
    std::string error;
    expect(sender.offerSend(id, "中文.txt", body.size(),
                            [&](uint64_t offset, uint8_t* dst, size_t len, size_t* got, std::string*) {
                                if (offset >= body.size()) return false;
                                size_t n = std::min(len, body.size() - static_cast<size_t>(offset));
                                std::memcpy(dst, body.data() + offset, n);
                                *got = n;
                                return true;
                            },
                            &error),
           "发起发送");
    for (int i = 0; i < 30; ++i) {
        sender.tick(1000 + static_cast<uint64_t>(i) * 500);
        receiver.tick(1000 + static_cast<uint64_t>(i) * 500);
        left.pump();
        right.pump();
    }
    expect(offer, "收到文件请求");
    expect(finished, "接收完成");
    expect(received == body, "内容一致");

    p2p::FileTransferEngine rejector({
        [](const uint8_t*, size_t) { return p2p::FileTransferEngine::kSendOk; },
        [](const std::string&, uint64_t) {},
        [](bool, const std::string&) {},
        [](bool, const std::string&, uint64_t, uint64_t) {},
        [](bool, bool, const std::string&) {},
        [] {},
        [](const std::string&) {},
    });
    bool rejected = false;
    p2p::FileTransferEngine asker({
        [&](const uint8_t* data, size_t len) {
            rejector.onDatagram(data, len, 1000);
            return p2p::FileTransferEngine::kSendOk;
        },
        [](const std::string&, uint64_t) {},
        [&](bool accepted, const std::string&) { rejected = !accepted; },
        [](bool, const std::string&, uint64_t, uint64_t) {},
        [](bool, bool, const std::string&) {},
        [] {},
        [](const std::string&) {},
    });
    // 拒绝路径需要双向转发。这里直接让接收端在回调里拒绝，发送端同步收到拒绝帧。
    p2p::FileTransferEngine* askerPtr = &asker;
    p2p::FileTransferEngine peer({
        [askerPtr](const uint8_t* data, size_t len) {
            askerPtr->onDatagram(data, len, 1000);
            return p2p::FileTransferEngine::kSendOk;
        },
        [](const std::string&, uint64_t) {},
        [](bool, const std::string&) {},
        [](bool, const std::string&, uint64_t, uint64_t) {},
        [](bool, bool, const std::string&) {},
        [] {},
        [](const std::string&) {},
    });
    p2p::FileTransferEngine* peerPtr = &peer;
    p2p::FileTransferEngine asker2({
        [peerPtr](const uint8_t* data, size_t len) {
            peerPtr->onDatagram(data, len, 2000);
            return p2p::FileTransferEngine::kSendOk;
        },
        [](const std::string&, uint64_t) {},
        [&](bool accepted, const std::string&) { rejected = !accepted; },
        [](bool, const std::string&, uint64_t, uint64_t) {},
        [](bool, bool, const std::string&) {},
        [] {},
        [](const std::string&) {},
    });
    (void)rejector;
    (void)asker;
    peerPtr = &peer;
    // 重新接好发送回调：上面 asker2 已经指向 peer。peer 的发送回调仍指向 asker。改成 asker2。
    // 为避免复杂重接，下面用一对新的引擎。
    bool rejected2 = false;
    p2p::FileTransferEngine* b = nullptr;
    p2p::FileTransferEngine a({
        [&](const uint8_t* data, size_t len) {
            if (b) b->onDatagram(data, len, 3000);
            return p2p::FileTransferEngine::kSendOk;
        },
        [](const std::string&, uint64_t) {},
        [&](bool accepted, const std::string&) { rejected2 = !accepted; },
        [](bool, const std::string&, uint64_t, uint64_t) {},
        [](bool, bool, const std::string&) {},
        [] {},
        [](const std::string&) {},
    });
    p2p::FileTransferEngine bEngine({
        [&](const uint8_t* data, size_t len) {
            a.onDatagram(data, len, 3000);
            return p2p::FileTransferEngine::kSendOk;
        },
        [&](const std::string&, uint64_t) { bEngine.rejectReceive("用户拒绝接收"); },
        [](bool, const std::string&) {},
        [](bool, const std::string&, uint64_t, uint64_t) {},
        [](bool, bool, const std::string&) {},
        [] {},
        [](const std::string&) {},
    });
    b = &bEngine;
    p2p::TransferId id2{};
    id2.bytes[0] = 2;
    a.tick(3000);
    expect(a.offerSend(id2, "a.bin", 1,
                       [](uint64_t, uint8_t* dst, size_t, size_t* got, std::string*) {
                           dst[0] = 1;
                           *got = 1;
                           return true;
                       },
                       &error),
           "拒绝前可以发出请求");
    expect(rejected2, "接收端拒绝");

    p2p::FileTransferEngine* zeroB = nullptr;
    bool zeroDone = false;
    p2p::FileTransferEngine zeroA({
        [&](const uint8_t* data, size_t len) {
            if (zeroB) zeroB->onDatagram(data, len, 4000);
            return p2p::FileTransferEngine::kSendOk;
        },
        [](const std::string&, uint64_t) {},
        [](bool, const std::string&) {},
        [](bool, const std::string&, uint64_t, uint64_t) {},
        [](bool, bool, const std::string&) {},
        [] {},
        [](const std::string&) {},
    });
    p2p::FileTransferEngine zeroBEngine({
        [&](const uint8_t* data, size_t len) {
            zeroA.onDatagram(data, len, 4000);
            return p2p::FileTransferEngine::kSendOk;
        },
        [&](const std::string&, uint64_t size) {
            expect(size == 0, "零字节大小");
            zeroBEngine.acceptReceive([](const uint8_t*, size_t, std::string*) { return true; }, [] {});
        },
        [](bool, const std::string&) {},
        [](bool, const std::string&, uint64_t, uint64_t) {},
        [&](bool sending, bool ok, const std::string&) {
            if (!sending && ok) zeroDone = true;
        },
        [] {},
        [](const std::string&) {},
    });
    zeroB = &zeroBEngine;
    p2p::TransferId id3{};
    id3.bytes[0] = 3;
    zeroA.tick(4000);
    expect(zeroA.offerSend(id3, "empty.dat", 0,
                           [](uint64_t, uint8_t*, size_t, size_t*, std::string*) { return false; }, &error),
           "发送零字节文件");
    expect(zeroDone, "零字节接收完成");
}

}  // namespace

int main() {
    std::cout.setf(std::ios::unitbuf);
    try {
    testCrc();
    testSha();
    testFragment();
    testNames();
    testTransfer();
    } catch (const std::exception& ex) {
        std::cout << "异常 " << ex.what() << "\n";
        return 2;
    }
    if (g_failed) {
        std::cout << "失败数量 " << g_failed << "\n";
        return 1;
    }
    std::cout << "全部协议自测通过\n";
    return 0;
}
