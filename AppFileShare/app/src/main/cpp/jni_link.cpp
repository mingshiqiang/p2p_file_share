#include "p2p/LinkCore.h"
#include "protocol/FileName.h"
#include "protocol/TimeUtil.h"

#include <jni.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

struct JObject {
    JavaVM* vm = nullptr;
    jobject object = nullptr;
    ~JObject() { reset(); }
    void reset() {
        if (!vm || !object) {
            object = nullptr;
            return;
        }
        JNIEnv* env = nullptr;
        bool attached = false;
        if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
            if (vm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
                object = nullptr;
                return;
            }
            attached = true;
        }
        env->DeleteGlobalRef(object);
        object = nullptr;
        if (attached) vm->DetachCurrentThread();
    }
};

struct NativeContext {
    JavaVM* vm = nullptr;
    jobject listener = nullptr;
    jmethodID onSendBle = nullptr;
    jmethodID onState = nullptr;
    jmethodID onLog = nullptr;
    jmethodID onOffer = nullptr;
    jmethodID onProgress = nullptr;
    jmethodID onFinished = nullptr;
    jmethodID onPeerDecision = nullptr;
    jmethodID onPeer = nullptr;
    jmethodID readerRead = nullptr;
    jmethodID writerWrite = nullptr;
    jmethodID writerDiscard = nullptr;

    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> queue;
    bool stop = false;
    std::thread worker;
    std::unique_ptr<p2p::LinkCore> core;

    void post(std::function<void()> fn) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stop) return;
            queue.push_back(std::move(fn));
        }
        cv.notify_one();
    }
};

std::string fromJstring(JNIEnv* env, jstring value) {
    if (!value) return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string out = chars ? chars : "";
    if (chars) env->ReleaseStringUTFChars(value, chars);
    return out;
}

std::string exceptionText(JNIEnv* env) {
    if (!env->ExceptionCheck()) return {};
    jthrowable thrown = env->ExceptionOccurred();
    env->ExceptionClear();
    if (!thrown) return "Java 异常";
    jclass clazz = env->GetObjectClass(thrown);
    jmethodID toString = env->GetMethodID(clazz, "toString", "()Ljava/lang/String;");
    auto* text = static_cast<jstring>(env->CallObjectMethod(thrown, toString));
    std::string out = fromJstring(env, text);
    env->DeleteLocalRef(text);
    env->DeleteLocalRef(clazz);
    env->DeleteLocalRef(thrown);
    return out.empty() ? "Java 异常" : out;
}

JNIEnv* envOf(JavaVM* vm) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) return nullptr;
    return env;
}

void callSendBle(NativeContext* ctx, const std::vector<uint8_t>& packet) {
    JNIEnv* env = envOf(ctx->vm);
    if (!env || !ctx->listener || !ctx->onSendBle) return;
    jbyteArray array = env->NewByteArray(static_cast<jsize>(packet.size()));
    if (!packet.empty()) {
        env->SetByteArrayRegion(array, 0, static_cast<jsize>(packet.size()),
                                reinterpret_cast<const jbyte*>(packet.data()));
    }
    env->CallVoidMethod(ctx->listener, ctx->onSendBle, array);
    env->DeleteLocalRef(array);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

void callLog(NativeContext* ctx, const std::string& line) {
    JNIEnv* env = envOf(ctx->vm);
    if (!env || !ctx->listener || !ctx->onLog) return;
    jstring text = env->NewStringUTF(line.c_str());
    env->CallVoidMethod(ctx->listener, ctx->onLog, text);
    env->DeleteLocalRef(text);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

bool readFromJava(NativeContext* ctx, jobject reader, uint64_t offset, uint8_t* dst, size_t len, size_t* got,
                  std::string* error) {
    JNIEnv* env = envOf(ctx->vm);
    if (!env || !reader) {
        if (error) *error = "文件读取器不可用";
        return false;
    }
    auto* bytes = static_cast<jbyteArray>(env->CallObjectMethod(reader, ctx->readerRead, static_cast<jlong>(offset),
                                                                static_cast<jint>(len)));
    if (env->ExceptionCheck()) {
        if (error) *error = exceptionText(env);
        return false;
    }
    if (!bytes) {
        if (error) *error = "读取文件失败";
        return false;
    }
    const jsize n = env->GetArrayLength(bytes);
    if (n < 0 || static_cast<size_t>(n) > len) {
        env->DeleteLocalRef(bytes);
        if (error) *error = "读取长度异常";
        return false;
    }
    if (n > 0) env->GetByteArrayRegion(bytes, 0, n, reinterpret_cast<jbyte*>(dst));
    env->DeleteLocalRef(bytes);
    *got = static_cast<size_t>(n);
    return true;
}

bool writeToJava(NativeContext* ctx, const std::shared_ptr<JObject>& writer, const uint8_t* data, size_t len,
                 std::string* error) {
    JNIEnv* env = envOf(ctx->vm);
    if (!env || !writer || !writer->object) {
        if (error) *error = "文件写入器不可用";
        return false;
    }
    jbyteArray bytes = env->NewByteArray(static_cast<jsize>(len));
    if (len) env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(len), reinterpret_cast<const jbyte*>(data));
    env->CallVoidMethod(writer->object, ctx->writerWrite, bytes);
    env->DeleteLocalRef(bytes);
    if (env->ExceptionCheck()) {
        if (error) *error = exceptionText(env);
        return false;
    }
    return true;
}

}  // namespace

extern "C" JNIEXPORT jlong JNICALL Java_com_example_appfileshare_NativeLink_nativeCreate(JNIEnv* env, jclass,
                                                                                         jobject listener,
                                                                                         jstring deviceId,
                                                                                         jstring deviceName) {
    auto* ctx = new NativeContext();
    env->GetJavaVM(&ctx->vm);
    ctx->listener = env->NewGlobalRef(listener);
    jclass listenerClass = env->GetObjectClass(listener);
    ctx->onSendBle = env->GetMethodID(listenerClass, "onSendBle", "([B)V");
    ctx->onState = env->GetMethodID(listenerClass, "onState", "(ILjava/lang/String;)V");
    ctx->onLog = env->GetMethodID(listenerClass, "onLog", "(Ljava/lang/String;)V");
    ctx->onOffer = env->GetMethodID(listenerClass, "onOffer", "(Ljava/lang/String;J)V");
    ctx->onProgress = env->GetMethodID(listenerClass, "onProgress", "(ZLjava/lang/String;JJ)V");
    ctx->onFinished = env->GetMethodID(listenerClass, "onFinished", "(ZZLjava/lang/String;)V");
    ctx->onPeerDecision = env->GetMethodID(listenerClass, "onPeerDecision", "(ZLjava/lang/String;)V");
    ctx->onPeer = env->GetMethodID(listenerClass, "onPeer", "(Ljava/lang/String;Ljava/lang/String;)V");
    env->DeleteLocalRef(listenerClass);
    jclass readerClass = env->FindClass("com/example/appfileshare/NativeLink$Reader");
    jclass writerClass = env->FindClass("com/example/appfileshare/NativeLink$Writer");
    ctx->readerRead = env->GetMethodID(readerClass, "read", "(JI)[B");
    ctx->writerWrite = env->GetMethodID(writerClass, "write", "([B)V");
    ctx->writerDiscard = env->GetMethodID(writerClass, "discard", "()V");
    env->DeleteLocalRef(readerClass);
    env->DeleteLocalRef(writerClass);
    if (!ctx->onSendBle || !ctx->readerRead || !ctx->writerWrite) {
        env->DeleteGlobalRef(ctx->listener);
        delete ctx;
        return 0;
    }

    p2p::LinkCore::Callbacks callbacks;
    callbacks.sendBle = [ctx](std::vector<uint8_t> packet) { callSendBle(ctx, packet); };
    callbacks.onState = [ctx](int state, std::string detail) {
        JNIEnv* jni = envOf(ctx->vm);
        if (!jni) return;
        jstring text = jni->NewStringUTF(detail.c_str());
        jni->CallVoidMethod(ctx->listener, ctx->onState, state, text);
        jni->DeleteLocalRef(text);
        if (jni->ExceptionCheck()) jni->ExceptionClear();
    };
    callbacks.onLog = [ctx](std::string line) { callLog(ctx, line); };
    callbacks.onOffer = [ctx](std::string name, uint64_t size) {
        JNIEnv* jni = envOf(ctx->vm);
        if (!jni) return;
        jstring text = jni->NewStringUTF(name.c_str());
        jni->CallVoidMethod(ctx->listener, ctx->onOffer, text, static_cast<jlong>(size));
        jni->DeleteLocalRef(text);
        if (jni->ExceptionCheck()) jni->ExceptionClear();
    };
    callbacks.onProgress = [ctx](bool sending, std::string name, uint64_t done, uint64_t total) {
        JNIEnv* jni = envOf(ctx->vm);
        if (!jni) return;
        jstring text = jni->NewStringUTF(name.c_str());
        jni->CallVoidMethod(ctx->listener, ctx->onProgress, static_cast<jboolean>(sending), text,
                            static_cast<jlong>(done), static_cast<jlong>(total));
        jni->DeleteLocalRef(text);
        if (jni->ExceptionCheck()) jni->ExceptionClear();
    };
    callbacks.onFinished = [ctx](bool sending, bool ok, std::string message) {
        JNIEnv* jni = envOf(ctx->vm);
        if (!jni) return;
        jstring text = jni->NewStringUTF(message.c_str());
        jni->CallVoidMethod(ctx->listener, ctx->onFinished, static_cast<jboolean>(sending), static_cast<jboolean>(ok),
                            text);
        jni->DeleteLocalRef(text);
        if (jni->ExceptionCheck()) jni->ExceptionClear();
    };
    callbacks.onPeerDecision = [ctx](bool accepted, std::string reason) {
        JNIEnv* jni = envOf(ctx->vm);
        if (!jni) return;
        jstring text = jni->NewStringUTF(reason.c_str());
        jni->CallVoidMethod(ctx->listener, ctx->onPeerDecision, static_cast<jboolean>(accepted), text);
        jni->DeleteLocalRef(text);
        if (jni->ExceptionCheck()) jni->ExceptionClear();
    };
    callbacks.onPeer = [ctx](std::string name, std::string id) {
        JNIEnv* jni = envOf(ctx->vm);
        if (!jni) return;
        jstring jname = jni->NewStringUTF(name.c_str());
        jstring jid = jni->NewStringUTF(id.c_str());
        jni->CallVoidMethod(ctx->listener, ctx->onPeer, jname, jid);
        jni->DeleteLocalRef(jname);
        jni->DeleteLocalRef(jid);
        if (jni->ExceptionCheck()) jni->ExceptionClear();
    };

    ctx->core = std::make_unique<p2p::LinkCore>(p2p::Role::AndroidPeripheral, fromJstring(env, deviceId),
                                                fromJstring(env, deviceName), std::move(callbacks));
    ctx->core->setPoster([ctx](std::function<void()> fn) { ctx->post(std::move(fn)); });
    ctx->worker = std::thread([ctx] {
        JNIEnv* jni = nullptr;
        ctx->vm->AttachCurrentThread(&jni, nullptr);
        while (true) {
            std::deque<std::function<void()>> local;
            bool stopping = false;
            {
                std::unique_lock<std::mutex> lock(ctx->mutex);
                ctx->cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return ctx->stop || !ctx->queue.empty(); });
                stopping = ctx->stop;
                local.swap(ctx->queue);
            }
            for (auto& fn : local) fn();
            if (stopping) break;
            ctx->core->tick(p2p::monotonicMs());
        }
        ctx->core->onTransportDown("原生层已关闭");
        ctx->core.reset();
        ctx->vm->DetachCurrentThread();
    });
    return reinterpret_cast<jlong>(ctx);
}

extern "C" JNIEXPORT void JNICALL Java_com_example_appfileshare_NativeLink_nativeDestroy(JNIEnv* env, jclass, jlong handle) {
    auto* ctx = reinterpret_cast<NativeContext*>(handle);
    if (!ctx) return;
    {
        std::lock_guard<std::mutex> lock(ctx->mutex);
        ctx->stop = true;
    }
    ctx->cv.notify_one();
    if (ctx->worker.joinable()) ctx->worker.join();
    if (ctx->listener) env->DeleteGlobalRef(ctx->listener);
    ctx->listener = nullptr;
    delete ctx;
}

extern "C" JNIEXPORT void JNICALL Java_com_example_appfileshare_NativeLink_nativeTransportUp(JNIEnv*, jclass, jlong handle,
                                                                                           jint attPayload) {
    auto* ctx = reinterpret_cast<NativeContext*>(handle);
    if (!ctx) return;
    ctx->post([ctx, attPayload] {
        ctx->core->setAttPayload(static_cast<size_t>(attPayload));
        ctx->core->onTransportUp(p2p::monotonicMs());
    });
}

extern "C" JNIEXPORT void JNICALL Java_com_example_appfileshare_NativeLink_nativeTransportDown(JNIEnv* env, jclass,
                                                                                              jlong handle,
                                                                                              jstring reason) {
    auto* ctx = reinterpret_cast<NativeContext*>(handle);
    if (!ctx) return;
    const std::string text = fromJstring(env, reason);
    ctx->post([ctx, text] { ctx->core->onTransportDown(text); });
}

extern "C" JNIEXPORT void JNICALL Java_com_example_appfileshare_NativeLink_nativePacket(JNIEnv* env, jclass, jlong handle,
                                                                                       jbyteArray packet) {
    auto* ctx = reinterpret_cast<NativeContext*>(handle);
    if (!ctx || !packet) return;
    const jsize n = env->GetArrayLength(packet);
    std::vector<uint8_t> bytes(static_cast<size_t>(n));
    if (n > 0) env->GetByteArrayRegion(packet, 0, n, reinterpret_cast<jbyte*>(bytes.data()));
    ctx->post([ctx, bytes = std::move(bytes)] {
        ctx->core->onBlePacket(bytes.data(), bytes.size(), p2p::monotonicMs());
    });
}

extern "C" JNIEXPORT void JNICALL Java_com_example_appfileshare_NativeLink_nativeSetAttPayload(JNIEnv*, jclass,
                                                                                              jlong handle, jint payload) {
    auto* ctx = reinterpret_cast<NativeContext*>(handle);
    if (!ctx) return;
    ctx->post([ctx, payload] { ctx->core->setAttPayload(static_cast<size_t>(payload)); });
}

extern "C" JNIEXPORT jstring JNICALL Java_com_example_appfileshare_NativeLink_nativeQueueFile(JNIEnv* env, jclass,
                                                                                             jlong handle, jstring name,
                                                                                             jlong size, jobject reader) {
    auto* ctx = reinterpret_cast<NativeContext*>(handle);
    if (!ctx || !reader) return env->NewStringUTF("没有文件读取器");
    auto ref = std::make_shared<JObject>();
    ref->vm = ctx->vm;
    ref->object = env->NewGlobalRef(reader);
    const std::string safeName = fromJstring(env, name);
    std::string safe;
    std::string sanitizeError;
    if (!p2p::sanitizeFileName(safeName, &safe, &sanitizeError)) {
        return env->NewStringUTF(sanitizeError.c_str());
    }
    std::mutex doneMutex;
    std::condition_variable doneCv;
    bool done = false;
    std::string result;
    ctx->post([ctx, ref, safe, size, &doneMutex, &doneCv, &done, &result] {
        std::string queueError;
        const bool ok = ctx->core->queueFile(
            safe, static_cast<uint64_t>(size),
            [ctx, ref](uint64_t offset, uint8_t* dst, size_t len, size_t* got, std::string* err) {
                return readFromJava(ctx, ref->object, offset, dst, len, got, err);
            },
            &queueError);
        std::lock_guard<std::mutex> lock(doneMutex);
        result = ok ? std::string() : queueError;
        done = true;
        doneCv.notify_one();
    });
    std::unique_lock<std::mutex> lock(doneMutex);
    doneCv.wait(lock, [&] { return done; });
    return result.empty() ? nullptr : env->NewStringUTF(result.c_str());
}

extern "C" JNIEXPORT void JNICALL Java_com_example_appfileshare_NativeLink_nativeAccept(JNIEnv* env, jclass, jlong handle,
                                                                                       jobject writer) {
    auto* ctx = reinterpret_cast<NativeContext*>(handle);
    if (!ctx || !writer) return;
    auto ref = std::make_shared<JObject>();
    ref->vm = ctx->vm;
    ref->object = env->NewGlobalRef(writer);
    ctx->post([ctx, ref] {
        ctx->core->acceptFile(
            [ctx, ref](const uint8_t* data, size_t len, std::string* error) {
                return writeToJava(ctx, ref, data, len, error);
            },
            [ctx, ref] {
                JNIEnv* jni = envOf(ctx->vm);
                if (!jni || !ref->object) return;
                jni->CallVoidMethod(ref->object, ctx->writerDiscard);
                if (jni->ExceptionCheck()) jni->ExceptionClear();
            });
    });
}

extern "C" JNIEXPORT void JNICALL Java_com_example_appfileshare_NativeLink_nativeReject(JNIEnv* env, jclass, jlong handle,
                                                                                       jstring reason) {
    auto* ctx = reinterpret_cast<NativeContext*>(handle);
    if (!ctx) return;
    const std::string text = fromJstring(env, reason);
    ctx->post([ctx, text] { ctx->core->rejectFile(text); });
}

extern "C" JNIEXPORT void JNICALL Java_com_example_appfileshare_NativeLink_nativeCancelSend(JNIEnv*, jclass, jlong handle) {
    auto* ctx = reinterpret_cast<NativeContext*>(handle);
    if (!ctx) return;
    ctx->post([ctx] { ctx->core->cancelSend("用户取消发送"); });
}

extern "C" JNIEXPORT void JNICALL Java_com_example_appfileshare_NativeLink_nativeCancelReceive(JNIEnv*, jclass,
                                                                                              jlong handle) {
    auto* ctx = reinterpret_cast<NativeContext*>(handle);
    if (!ctx) return;
    ctx->post([ctx] { ctx->core->cancelReceive("用户取消接收"); });
}
