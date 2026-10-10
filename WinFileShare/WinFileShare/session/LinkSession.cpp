#include "session/LinkSession.h"

#include "ble/BleCentral.h"
#include "p2p/LinkCore.h"
#include "protocol/FileName.h"
#include "protocol/TimeUtil.h"

#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QSettings>
#include <QTimer>
#include <QUuid>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <windows.h>

namespace {

QString deviceId() {
    QSettings settings("P2PFileShare", "WinFileShare");
    QString id = settings.value("deviceId").toString();
    if (id.isEmpty()) {
        id = "WFS-" + QUuid::createUuid().toString(QUuid::Id128).left(16);
        settings.setValue("deviceId", id);
    }
    return id;
}

QString computerName() {
    wchar_t buffer[256];
    DWORD size = 256;
    if (!GetComputerNameW(buffer, &size)) return "Windows";
    return QString::fromWCharArray(buffer);
}

}  // namespace

struct LinkSession::CoreHolder {
    std::unique_ptr<p2p::LinkCore> core;
};

LinkSession::LinkSession(BleCentral* ble, QObject* parent) : QObject(parent), ble_(ble), core_(new CoreHolder) {}

void LinkSession::init() {
    const auto id = deviceId().toUtf8();
    const auto name = computerName().toUtf8();
    core_->core = std::make_unique<p2p::LinkCore>(
        p2p::Role::WindowsCentral, std::string(id.constData(), id.size()), std::string(name.constData(), name.size()),
        p2p::LinkCore::Callbacks{
            [this](std::vector<uint8_t> packet) {
                QByteArray bytes(reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()));
                QMetaObject::invokeMethod(ble_, [this, bytes] {
                    if (ble_) ble_->sendPacket(bytes);
                }, Qt::QueuedConnection);
            },
            [this](int state, std::string detail) { emit stateChanged(state, QString::fromUtf8(detail.c_str())); },
            [this](std::string line) { emit logMessage(QString::fromUtf8(line.c_str())); },
            [this](std::string fileName, uint64_t size) {
                emit incomingOffer(QString::fromUtf8(fileName.c_str()), static_cast<quint64>(size));
            },
            [this](bool sending, std::string fileName, uint64_t done, uint64_t total) {
                emit progressChanged(sending, QString::fromUtf8(fileName.c_str()), static_cast<quint64>(done),
                                     static_cast<quint64>(total));
            },
            [this](bool sending, bool ok, std::string message) {
                emit transferFinished(sending, ok, QString::fromUtf8(message.c_str()));
            },
            [this](bool accepted, std::string reason) {
                emit peerDecision(accepted, QString::fromUtf8(reason.c_str()));
            },
            [this](std::string name, std::string id) {
                emit peerChanged(QString::fromUtf8(name.c_str()), QString::fromUtf8(id.c_str()));
            },
        });
    core_->core->setPoster([this](std::function<void()> fn) {
        QMetaObject::invokeMethod(this, [fn = std::move(fn)] { fn(); }, Qt::QueuedConnection);
    });
    timer_ = new QTimer(this);
    connect(timer_, &QTimer::timeout, this, &LinkSession::onTick);
    timer_->start(100);
    emit logMessage("本机是未加密的学习原型，请不要传输敏感文件。设备 ID：" + deviceId());
}

void LinkSession::shutdown() {
    if (timer_) timer_->stop();
    if (core_ && core_->core) core_->core->onTransportDown("应用正在退出");
    if (ble_) {
        QMetaObject::invokeMethod(ble_, [this] {
            if (ble_) ble_->disconnectDevice();
        }, Qt::QueuedConnection);
    }
    delete core_;
    core_ = nullptr;
}

void LinkSession::onTransportUp(int attPayload) {
    if (!core_ || !core_->core) return;
    core_->core->setAttPayload(static_cast<size_t>(attPayload));
    core_->core->onTransportUp(p2p::monotonicMs());
}

void LinkSession::onTransportDown(const QString& reason) {
    if (!core_ || !core_->core) return;
    core_->core->onTransportDown(reason.toUtf8().toStdString());
}

void LinkSession::onPacket(const QByteArray& packet) {
    if (!core_ || !core_->core) return;
    core_->core->onBlePacket(reinterpret_cast<const uint8_t*>(packet.constData()), static_cast<size_t>(packet.size()),
                             p2p::monotonicMs());
}

void LinkSession::onAttPayload(int attPayload) {
    if (!core_ || !core_->core) return;
    core_->core->setAttPayload(static_cast<size_t>(attPayload));
}

void LinkSession::sendFile(const QString& path) {
    if (!core_ || !core_->core) return;
    auto file = std::make_shared<QFile>(path);
    if (!file->open(QIODevice::ReadOnly)) {
        emit transferFinished(true, false, "无法读取文件：" + file->errorString());
        return;
    }
    const auto size = static_cast<uint64_t>(file->size());
    std::string safe;
    std::string error;
    if (!p2p::sanitizeFileName(QFileInfo(path).fileName().toUtf8().toStdString(), &safe, &error)) {
        emit transferFinished(true, false, QString::fromUtf8(error.c_str()));
        return;
    }
    auto read = [file](uint64_t offset, uint8_t* dst, size_t len, size_t* got, std::string* err) {
        if (!file->seek(static_cast<qint64>(offset))) {
            if (err) *err = "无法定位发送文件";
            return false;
        }
        const qint64 n = file->read(reinterpret_cast<char*>(dst), static_cast<qint64>(len));
        if (n < 0) {
            if (err) *err = file->errorString().toUtf8().toStdString();
            return false;
        }
        *got = static_cast<size_t>(n);
        return true;
    };
    if (!core_->core->queueFile(safe, size, read, &error)) {
        emit transferFinished(true, false, QString::fromUtf8(error.c_str()));
    }
}

void LinkSession::acceptFile(const QString& path) {
    if (!core_ || !core_->core) return;
    auto file = std::make_shared<QFile>(path);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit transferFinished(false, false, "无法创建接收文件：" + file->errorString());
        core_->core->rejectFile("无法创建接收文件");
        return;
    }
    auto write = [file](const uint8_t* data, size_t len, std::string* err) {
        const qint64 n = file->write(reinterpret_cast<const char*>(data), static_cast<qint64>(len));
        if (n != static_cast<qint64>(len)) {
            if (err) *err = file->errorString().isEmpty() ? "写入文件失败，可能是磁盘空间不足或文件被占用"
                                                         : file->errorString().toUtf8().toStdString();
            return false;
        }
        return true;
    };
    auto discard = [file, path] {
        file->close();
        QFile::remove(path);
    };
    core_->core->acceptFile(write, discard);
}

void LinkSession::rejectFile(const QString& reason) {
    if (core_ && core_->core) core_->core->rejectFile(reason.toUtf8().toStdString());
}

void LinkSession::cancelSend() {
    if (core_ && core_->core) core_->core->cancelSend("用户取消发送");
}

void LinkSession::cancelReceive() {
    if (core_ && core_->core) core_->core->cancelReceive("用户取消接收");
}

void LinkSession::onTick() {
    if (core_ && core_->core) core_->core->tick(p2p::monotonicMs());
}
