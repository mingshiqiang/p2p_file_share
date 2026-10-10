#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace p2p {

class JuiceSession {
public:
    struct Handlers {
        // 回调在 libjuice 内部线程执行，只能转发事件，不能销毁 agent，也不能操作界面。
        std::function<void(int state, std::string name)> onState;
        std::function<void(std::string candidate)> onCandidate;
        std::function<void()> onGatheringDone;
        std::function<void(std::vector<uint8_t> data)> onRecv;
        std::function<void(std::string line)> onLog;
    };

    JuiceSession();
    ~JuiceSession();

    JuiceSession(const JuiceSession&) = delete;
    JuiceSession& operator=(const JuiceSession&) = delete;

    bool create(Handlers handlers, std::string* error);
    void destroy();
    bool valid() const;

    bool localDescription(std::string* sdp, std::string* error);
    bool setRemoteDescription(const std::string& sdp, std::string* error);
    bool gatherCandidates(std::string* error);
    bool addRemoteCandidate(const std::string& sdp, std::string* error);
    bool setRemoteGatheringDone(std::string* error);
    int send(const uint8_t* data, size_t size);
    int state() const;
    std::string selectedSummary() const;

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace p2p
