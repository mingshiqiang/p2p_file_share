#include "ble/BleCentral.h"

#include "protocol/Ids.h"

#include <QDateTime>
#include <QPointer>
#include <QTimer>

#include <algorithm>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>

using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Devices::Radios;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage::Streams;

namespace {

QString fromHstring(winrt::hstring const& value) { return QString::fromWCharArray(value.c_str()); }

winrt::guid serviceGuid() {
    return {0x6b8a1f40, 0x2c7d, 0x4e15, {0x9a, 0x33, 0x0d, 0x5e, 0x7c, 0x1b, 0x4a, 0x86}};
}
winrt::guid rxGuid() {
    return {0x6b8a1f41, 0x2c7d, 0x4e15, {0x9a, 0x33, 0x0d, 0x5e, 0x7c, 0x1b, 0x4a, 0x86}};
}
winrt::guid txGuid() {
    return {0x6b8a1f42, 0x2c7d, 0x4e15, {0x9a, 0x33, 0x0d, 0x5e, 0x7c, 0x1b, 0x4a, 0x86}};
}

QString readServiceToken(BluetoothLEAdvertisement const& advertisement) {
    for (auto const& section : advertisement.DataSections()) {
        if (section.DataType() != 0x21) continue;
        auto reader = DataReader::FromBuffer(section.Data());
        const uint32_t length = reader.UnconsumedBufferLength();
        if (length < 16) continue;
        std::vector<uint8_t> bytes(length);
        reader.ReadBytes(winrt::array_view<uint8_t>(bytes));
        if (!std::equal(std::begin(p2p::kServiceUuidWire), std::end(p2p::kServiceUuidWire), bytes.begin())) continue;
        return QString::fromUtf8(reinterpret_cast<const char*>(bytes.data() + 16), static_cast<int>(length - 16));
    }
    return {};
}

}  // namespace

struct BleCentral::Impl {
    bool apartmentReady = false;
    QTimer* timer = nullptr;
    BluetoothLEAdvertisementWatcher watcher{nullptr};
    winrt::event_token receivedToken{};
    winrt::event_token stoppedToken{};
    bool scanning = false;

    BluetoothLEDevice device{nullptr};
    GattDeviceService service{nullptr};
    GattCharacteristic rx{nullptr};
    GattCharacteristic tx{nullptr};
    GattSession session{nullptr};
    winrt::event_token valueToken{};
    winrt::event_token connectionToken{};
    winrt::event_token pduToken{};
    bool subscribed = false;
    quint64 connectedAddress = 0;
    int attPayload = static_cast<int>(p2p::kDefaultAttPayload);

    std::deque<QByteArray> writes;
    bool writing = false;

    mutable std::mutex devicesMutex;
    struct Record {
        BleCentral::DeviceRow row;
        qint64 lastMs = 0;
    };
    std::vector<Record> devices;
};

BleCentral::BleCentral(QObject* parent) : QObject(parent), impl_(new Impl) {}

BleCentral::~BleCentral() {
    closeConnection({}, false);
    stopScan();
    delete impl_;
    impl_ = nullptr;
}

QVector<BleCentral::DeviceRow> BleCentral::snapshot() const {
    QVector<DeviceRow> rows;
    if (!impl_) return rows;
    std::lock_guard<std::mutex> lock(impl_->devicesMutex);
    rows.reserve(static_cast<int>(impl_->devices.size()));
    for (const auto& record : impl_->devices) rows.push_back(record.row);
    return rows;
}

void BleCentral::initOnThread() {
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        impl_->apartmentReady = true;
    } catch (winrt::hresult_error const& ex) {
        fail(QString("初始化蓝牙线程失败：%1").arg(fromHstring(ex.message())));
        return;
    }
    impl_->timer = new QTimer(this);
    connect(impl_->timer, &QTimer::timeout, this, &BleCentral::ageDevices);
    impl_->timer->start(1000);
}

void BleCentral::fail(const QString& message) {
    emit errorOccurred(message);
    emit logMessage(message);
}

void BleCentral::startScan() {
    if (!impl_->apartmentReady) {
        fail("蓝牙线程尚未就绪");
        return;
    }
    try {
        auto adapter = BluetoothAdapter::GetDefaultAsync().get();
        if (!adapter) {
            fail("这台电脑没有可用的蓝牙适配器");
            return;
        }
        if (!adapter.IsLowEnergySupported()) {
            fail("这台电脑不支持低功耗蓝牙");
            return;
        }
        auto radio = adapter.GetRadioAsync().get();
        if (!radio || radio.State() != RadioState::On) {
            fail("蓝牙未开启，请先在 Windows 设置中打开蓝牙");
            return;
        }
        if (impl_->scanning) return;
        BluetoothLEAdvertisementWatcher watcher;
        watcher.ScanningMode(BluetoothLEScanningMode::Active);
        watcher.AdvertisementFilter().Advertisement().ServiceUuids().Append(serviceGuid());
        QPointer<BleCentral> self(this);
        impl_->receivedToken = watcher.Received([self](BluetoothLEAdvertisementWatcher const&,
                                                       BluetoothLEAdvertisementReceivedEventArgs const& args) {
            if (!self || !self->impl_) return;
            DeviceRow row;
            row.address = args.BluetoothAddress();
            row.rssi = args.RawSignalStrengthInDBm();
            row.name = fromHstring(args.Advertisement().LocalName());
            row.deviceId = readServiceToken(args.Advertisement());
            row.lastSeen = QDateTime::currentDateTime().toString("HH:mm:ss");
            QMetaObject::invokeMethod(self, [self, row] {
                if (!self || !self->impl_) return;
                std::lock_guard<std::mutex> lock(self->impl_->devicesMutex);
                auto& devices = self->impl_->devices;
                auto it = std::find_if(devices.begin(), devices.end(),
                                       [&](const Impl::Record& item) { return item.row.address == row.address; });
                if (it == devices.end()) {
                    Impl::Record record;
                    record.row = row;
                    if (record.row.name.isEmpty()) record.row.name = "未命名手机";
                    if (record.row.deviceId.isEmpty()) record.row.deviceId = "连接后确认";
                    record.lastMs = QDateTime::currentMSecsSinceEpoch();
                    devices.push_back(record);
                } else {
                    it->lastMs = QDateTime::currentMSecsSinceEpoch();
                    it->row.rssi = row.rssi;
                    it->row.lastSeen = row.lastSeen;
                    it->row.stale = false;
                    if (!row.name.isEmpty()) it->row.name = row.name;
                    if (!row.deviceId.isEmpty()) it->row.deviceId = row.deviceId;
                }
                emit self->devicesChanged();
            }, Qt::QueuedConnection);
        });
        impl_->stoppedToken = watcher.Stopped([self](BluetoothLEAdvertisementWatcher const&,
                                                     BluetoothLEAdvertisementWatcherStoppedEventArgs const& args) {
            if (!self) return;
            const auto error = args.Error();
            QMetaObject::invokeMethod(self, [self, error] {
                if (!self) return;
                if (error != BluetoothError::Success) self->fail("蓝牙扫描已停止，系统返回错误");
                emit self->scanStateChanged(false);
            }, Qt::QueuedConnection);
        });
        watcher.Start();
        impl_->watcher = watcher;
        impl_->scanning = true;
        emit scanStateChanged(true);
        emit logMessage("已开始扫描带有本应用服务的手机");
    } catch (winrt::hresult_error const& ex) {
        fail(QString("启动蓝牙扫描失败：%1").arg(fromHstring(ex.message())));
    }
}

void BleCentral::stopScan() {
    if (!impl_ || !impl_->scanning) return;
    try {
        if (impl_->watcher) {
            impl_->watcher.Stop();
            impl_->watcher.Received(impl_->receivedToken);
            impl_->watcher.Stopped(impl_->stoppedToken);
            impl_->watcher = nullptr;
        }
    } catch (...) {
    }
    impl_->scanning = false;
    emit scanStateChanged(false);
    emit logMessage("已停止扫描");
}

void BleCentral::ageDevices() {
    if (!impl_) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool changed = false;
    std::lock_guard<std::mutex> lock(impl_->devicesMutex);
    for (auto it = impl_->devices.begin(); it != impl_->devices.end();) {
        const qint64 age = now - it->lastMs;
        if (it->row.address != impl_->connectedAddress && age > 120000) {
            it = impl_->devices.erase(it);
            changed = true;
            continue;
        }
        const bool stale = age > 10000 && it->row.address != impl_->connectedAddress;
        if (it->row.stale != stale) {
            it->row.stale = stale;
            changed = true;
        }
        ++it;
    }
    if (changed) emit devicesChanged();
}

void BleCentral::connectTo(quint64 address) {
    if (!impl_->apartmentReady) {
        fail("蓝牙线程尚未就绪");
        return;
    }
    closeConnection({}, false);
    try {
        auto device = BluetoothLEDevice::FromBluetoothAddressAsync(address).get();
        if (!device) {
            fail("找不到这台手机，它可能已经离开范围");
            return;
        }
        auto access = device.RequestAccessAsync().get();
        if (access != DeviceAccessStatus::Allowed) {
            fail("系统拒绝访问这台蓝牙设备");
            device.Close();
            return;
        }
        QPointer<BleCentral> self(this);
        impl_->connectionToken = device.ConnectionStatusChanged(
            [self](BluetoothLEDevice const& sender, winrt::Windows::Foundation::IInspectable const&) {
                if (!self) return;
                if (sender.ConnectionStatus() == BluetoothConnectionStatus::Disconnected) {
                    QMetaObject::invokeMethod(self, [self] {
                        if (self) self->closeConnection("蓝牙连接中断", true);
                    }, Qt::QueuedConnection);
                }
            });
        auto services = device.GetGattServicesForUuidAsync(serviceGuid(), BluetoothCacheMode::Uncached).get();
        if (services.Status() != GattCommunicationStatus::Success || services.Services().Size() == 0) {
            fail("已连上设备，但没有找到本应用的 GATT 服务");
            device.Close();
            return;
        }
        auto service = services.Services().GetAt(0);
        auto chars = service.GetCharacteristicsAsync(BluetoothCacheMode::Uncached).get();
        if (chars.Status() != GattCommunicationStatus::Success) {
            fail("读取 GATT 特征值失败");
            service.Close();
            device.Close();
            return;
        }
        GattCharacteristic rx{nullptr};
        GattCharacteristic tx{nullptr};
        for (auto const& characteristic : chars.Characteristics()) {
            if (characteristic.Uuid() == rxGuid()) rx = characteristic;
            if (characteristic.Uuid() == txGuid()) tx = characteristic;
        }
        if (!rx || !tx) {
            fail("GATT 服务中缺少接收或发送特征值");
            service.Close();
            device.Close();
            return;
        }
        auto notifyStatus = tx.WriteClientCharacteristicConfigurationDescriptorAsync(
                                  GattClientCharacteristicConfigurationDescriptorValue::Notify)
                                .get();
        if (notifyStatus != GattCommunicationStatus::Success) {
            fail("手机拒绝了蓝牙通知订阅，Notify 不可用");
            service.Close();
            device.Close();
            return;
        }
        impl_->valueToken = tx.ValueChanged([self](GattCharacteristic const&, GattValueChangedEventArgs const& args) {
            if (!self) return;
            auto reader = DataReader::FromBuffer(args.CharacteristicValue());
            const uint32_t length = reader.UnconsumedBufferLength();
            QByteArray bytes;
            bytes.resize(static_cast<int>(length));
            if (length) {
                std::vector<uint8_t> raw(length);
                reader.ReadBytes(winrt::array_view<uint8_t>(raw));
                std::memcpy(bytes.data(), raw.data(), length);
            }
            QMetaObject::invokeMethod(self, [self, bytes] {
                if (self) emit self->packetReceived(bytes);
            }, Qt::QueuedConnection);
        });
        impl_->device = device;
        impl_->service = service;
        impl_->rx = rx;
        impl_->tx = tx;
        impl_->subscribed = true;
        impl_->connectedAddress = address;
        try {
            impl_->session = GattSession::FromDeviceIdAsync(device.BluetoothDeviceId()).get();
            applyPayload(impl_->session.MaxPduSize() > 3 ? impl_->session.MaxPduSize() - 3
                                                         : static_cast<int>(p2p::kDefaultAttPayload));
            impl_->pduToken = impl_->session.MaxPduSizeChanged([self](GattSession const& session, auto const&) {
                if (!self) return;
                const int payload = session.MaxPduSize() > 3 ? static_cast<int>(session.MaxPduSize() - 3)
                                                            : static_cast<int>(p2p::kDefaultAttPayload);
                QMetaObject::invokeMethod(self, [self, payload] {
                    if (self) self->applyPayload(payload);
                }, Qt::QueuedConnection);
            });
        } catch (...) {
            applyPayload(static_cast<int>(p2p::kDefaultAttPayload));
        }
        emit logMessage(QString("蓝牙 GATT 已连接，当前写入载荷 %1 字节").arg(impl_->attPayload));
        emit transportUp(impl_->attPayload);
    } catch (winrt::hresult_error const& ex) {
        fail(QString("连接手机失败：%1").arg(fromHstring(ex.message())));
        closeConnection({}, false);
    }
}

void BleCentral::applyPayload(int payload) {
    if (payload < static_cast<int>(p2p::kBleHeaderSize + 1)) payload = static_cast<int>(p2p::kDefaultAttPayload);
    if (!impl_ || impl_->attPayload == payload) return;
    impl_->attPayload = payload;
    emit attPayloadChanged(payload);
    emit logMessage(QString("蓝牙 ATT 载荷更新为 %1 字节").arg(payload));
}

void BleCentral::disconnectDevice() { closeConnection("已断开蓝牙连接", true); }

void BleCentral::closeConnection(const QString& reason, bool notify) {
    if (!impl_) return;
    impl_->writes.clear();
    impl_->writing = false;
    try {
        if (impl_->tx && impl_->subscribed) impl_->tx.ValueChanged(impl_->valueToken);
        if (impl_->session && impl_->pduToken.value) impl_->session.MaxPduSizeChanged(impl_->pduToken);
        if (impl_->device && impl_->connectionToken.value) impl_->device.ConnectionStatusChanged(impl_->connectionToken);
        if (impl_->service) impl_->service.Close();
        if (impl_->device) impl_->device.Close();
    } catch (...) {
    }
    impl_->tx = nullptr;
    impl_->rx = nullptr;
    impl_->service = nullptr;
    impl_->device = nullptr;
    impl_->session = nullptr;
    impl_->subscribed = false;
    impl_->connectedAddress = 0;
    if (notify) emit transportDown(reason);
}

void BleCentral::sendPacket(const QByteArray& packet) {
    if (!impl_ || !impl_->rx) {
        fail("蓝牙还未连接，无法发送");
        return;
    }
    if (impl_->writes.size() >= 256) {
        fail("蓝牙发送队列已满");
        return;
    }
    impl_->writes.push_back(packet);
    pumpWrite();
}

void BleCentral::pumpWrite() {
    if (!impl_ || impl_->writing || impl_->writes.empty() || !impl_->rx) return;
    impl_->writing = true;
    const QByteArray packet = impl_->writes.front();
    try {
        DataWriter writer;
        std::vector<uint8_t> bytes(static_cast<size_t>(packet.size()));
        if (!packet.isEmpty()) std::memcpy(bytes.data(), packet.constData(), bytes.size());
        writer.WriteBytes(winrt::array_view<const uint8_t>(bytes));
        QPointer<BleCentral> self(this);
        auto operation = impl_->rx.WriteValueAsync(writer.DetachBuffer(), GattWriteOption::WriteWithResponse);
        operation.Completed([self](IAsyncOperation<GattCommunicationStatus> const& async, AsyncStatus status) {
            int result = static_cast<int>(GattCommunicationStatus::Unreachable);
            if (status == AsyncStatus::Completed) {
                try {
                    result = static_cast<int>(async.GetResults());
                } catch (...) {
                }
            }
            if (!self) return;
            QMetaObject::invokeMethod(self, [self, result] {
                if (self) self->finishWrite(result);
            }, Qt::QueuedConnection);
        });
    } catch (winrt::hresult_error const& ex) {
        impl_->writing = false;
        if (!impl_->writes.empty()) impl_->writes.pop_front();
        fail(QString("蓝牙写入失败：%1").arg(fromHstring(ex.message())));
    }
}

void BleCentral::finishWrite(int status) {
    if (!impl_) return;
    impl_->writing = false;
    if (!impl_->writes.empty()) impl_->writes.pop_front();
    if (static_cast<GattCommunicationStatus>(status) != GattCommunicationStatus::Success) {
        fail("蓝牙写入被拒绝或设备不可达");
        return;
    }
    pumpWrite();
}
