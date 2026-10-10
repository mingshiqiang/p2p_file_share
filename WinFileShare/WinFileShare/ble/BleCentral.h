#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <cstdint>

class BleCentral : public QObject {
    Q_OBJECT
public:
    struct DeviceRow {
        quint64 address = 0;
        QString name;
        QString deviceId;
        int rssi = 0;
        QString lastSeen;
        bool stale = false;
    };

    explicit BleCentral(QObject* parent = nullptr);
    ~BleCentral() override;

    QVector<DeviceRow> snapshot() const;

public slots:
    void initOnThread();
    void startScan();
    void stopScan();
    void connectTo(quint64 address);
    void disconnectDevice();
    void sendPacket(const QByteArray& packet);

signals:
    void logMessage(const QString& line);
    void errorOccurred(const QString& message);
    void scanStateChanged(bool running);
    void devicesChanged();
    void transportUp(int attPayload);
    void transportDown(const QString& reason);
    void packetReceived(const QByteArray& packet);
    void attPayloadChanged(int attPayload);

private slots:
    void ageDevices();
    void finishWrite(int status);

private:
    void fail(const QString& message);
    void pumpWrite();
    void closeConnection(const QString& reason, bool notify);
    void applyPayload(int payload);

    struct Impl;
    Impl* impl_ = nullptr;
};
