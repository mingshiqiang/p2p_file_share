#include "p2p/JuiceSession.h"

#include <juice/juice.h>

#include <cstring>
#include <mutex>
#include <vector>

namespace p2p {
namespace {

struct LogTarget {
    std::mutex mutex;
    bool alive = true;
    std::function<void(std::string)> onLog;
};

std::mutex g_logMutex;
std::vector<std::weak_ptr<LogTarget>> g_logTargets;
bool g_logInstalled = false;

void logThunk(juice_log_level_t, const char* message) {
    std::vector<std::shared_ptr<LogTarget>> live;
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        for (auto it = g_logTargets.begin(); it != g_logTargets.end();) {
            if (auto shared = it->lock()) {
                live.push_back(std::move(shared));
                ++it;
            } else {
                it = g_logTargets.erase(it);
            }
        }
    }
    const std::string text = message ? message : "";
    for (const auto& target : live) {
        std::function<void(std::string)> handler;
        {
            std::lock_guard<std::mutex> lock(target->mutex);
            if (target->alive) handler = target->onLog;
        }
        if (handler) handler(text);
    }
}

}  // namespace

struct JuiceSession::Impl {
    std::mutex mutex;
    bool alive = true;
    Handlers handlers;
    juice_agent_t* agent = nullptr;
    std::shared_ptr<LogTarget> logTarget;
};

JuiceSession::JuiceSession() = default;
JuiceSession::~JuiceSession() { destroy(); }
bool JuiceSession::valid() const { return impl_ && impl_->agent; }

bool JuiceSession::create(Handlers handlers, std::string* error) {
    destroy();
    impl_ = std::make_shared<Impl>();
    impl_->handlers = std::move(handlers);
    impl_->logTarget = std::make_shared<LogTarget>();
    impl_->logTarget->onLog = impl_->handlers.onLog;
    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        g_logTargets.push_back(impl_->logTarget);
        if (!g_logInstalled) {
            juice_set_log_level(JUICE_LOG_LEVEL_DEBUG);
            juice_set_log_handler(logThunk);
            g_logInstalled = true;
        }
    }

    juice_config_t config{};
    config.concurrency_mode = JUICE_CONCURRENCY_MODE_THREAD;
    // 第一版不配置 STUN/TURN。没有公网映射时，只能尝试本机直接可达的地址。
    config.stun_server_host = nullptr;
    config.stun_server_port = 0;
    config.user_ptr = impl_.get();
    config.cb_state_changed = [](juice_agent_t*, juice_state_t state, void* user) {
        auto* impl = static_cast<Impl*>(user);
        std::function<void(int, std::string)> handler;
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            if (!impl->alive) return;
            handler = impl->handlers.onState;
        }
        if (handler) handler(static_cast<int>(state), juice_state_to_string(state));
    };
    config.cb_candidate = [](juice_agent_t*, const char* sdp, void* user) {
        auto* impl = static_cast<Impl*>(user);
        std::function<void(std::string)> handler;
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            if (!impl->alive) return;
            handler = impl->handlers.onCandidate;
        }
        if (handler) handler(sdp ? sdp : "");
    };
    config.cb_gathering_done = [](juice_agent_t*, void* user) {
        auto* impl = static_cast<Impl*>(user);
        std::function<void()> handler;
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            if (!impl->alive) return;
            handler = impl->handlers.onGatheringDone;
        }
        if (handler) handler();
    };
    config.cb_recv = [](juice_agent_t*, const char* data, size_t size, void* user) {
        auto* impl = static_cast<Impl*>(user);
        std::function<void(std::vector<uint8_t>)> handler;
        {
            std::lock_guard<std::mutex> lock(impl->mutex);
            if (!impl->alive) return;
            handler = impl->handlers.onRecv;
        }
        if (!handler) return;
        std::vector<uint8_t> copy(size);
        if (size && data) std::memcpy(copy.data(), data, size);
        handler(std::move(copy));
    };

    impl_->agent = juice_create(&config);
    if (!impl_->agent) {
        if (error) *error = "无法创建 libjuice agent";
        destroy();
        return false;
    }
    return true;
}

void JuiceSession::destroy() {
    if (!impl_) return;
    juice_agent_t* agent = nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->alive = false;
        impl_->handlers = {};
        agent = impl_->agent;
        impl_->agent = nullptr;
    }
    if (impl_->logTarget) {
        std::lock_guard<std::mutex> lock(impl_->logTarget->mutex);
        impl_->logTarget->alive = false;
        impl_->logTarget->onLog = nullptr;
    }
    if (agent) juice_destroy(agent);
    impl_.reset();
}

bool JuiceSession::localDescription(std::string* sdp, std::string* error) {
    if (!valid() || !sdp) return false;
    char buffer[JUICE_MAX_SDP_STRING_LEN];
    buffer[0] = 0;
    if (juice_get_local_description(impl_->agent, buffer, sizeof(buffer)) != 0) {
        if (error) *error = "无法获取本地 SDP";
        return false;
    }
    *sdp = buffer;
    return true;
}

bool JuiceSession::setRemoteDescription(const std::string& sdp, std::string* error) {
    if (!valid()) return false;
    if (juice_set_remote_description(impl_->agent, sdp.c_str()) != 0) {
        if (error) *error = "无法设置远端 SDP";
        return false;
    }
    return true;
}

bool JuiceSession::gatherCandidates(std::string* error) {
    if (!valid()) return false;
    if (juice_gather_candidates(impl_->agent) != 0) {
        if (error) *error = "无法开始收集候选地址";
        return false;
    }
    return true;
}

bool JuiceSession::addRemoteCandidate(const std::string& sdp, std::string* error) {
    if (!valid()) return false;
    const int rc = juice_add_remote_candidate(impl_->agent, sdp.c_str());
    if (rc != 0 && rc != JUICE_ERR_IGNORED) {
        if (error) *error = "无法添加远端候选地址";
        return false;
    }
    return true;
}

bool JuiceSession::setRemoteGatheringDone(std::string* error) {
    if (!valid()) return false;
    if (juice_set_remote_gathering_done(impl_->agent) != 0) {
        if (error) *error = "无法标记远端候选收集完成";
        return false;
    }
    return true;
}

int JuiceSession::send(const uint8_t* data, size_t size) {
    if (!valid() || !data) return JUICE_ERR_INVALID;
    return juice_send(impl_->agent, reinterpret_cast<const char*>(data), size);
}

int JuiceSession::state() const {
    if (!valid()) return JUICE_STATE_DISCONNECTED;
    return static_cast<int>(juice_get_state(impl_->agent));
}

std::string JuiceSession::selectedSummary() const {
    if (!valid()) return {};
    char local[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
    char remote[JUICE_MAX_CANDIDATE_SDP_STRING_LEN];
    local[0] = remote[0] = 0;
    if (juice_get_selected_candidates(impl_->agent, local, sizeof(local), remote, sizeof(remote)) != 0) return {};
    return std::string("本地候选: ") + local + "；远端候选: " + remote;
}

}  // namespace p2p
