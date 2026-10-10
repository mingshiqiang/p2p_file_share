#pragma once

#include <QObject>
#include <QString>

class BleCentral;

class LinkSession : public QObject {
    Q_OBJECT
public:
    explicit LinkSession(BleCentral* ble, QObject* parent = nullptr);

public slots:
    void init();
    void shutdown();
    void onTransportUp(int attPayload);
    void onTransportDown(const QString& reason);
    void onPacket(const QByteArray& packet);
    void onAttPayload(int attPayload);
    void sendFile(const QString& path);
    void acceptFile(const QString& path);
    void rejectFile(const QString& reason);
    void cancelSend();
    void cancelReceive();

signals:
    void stateChanged(int state, const QString& detail);
    void logMessage(const QString& line);
    void incomingOffer(const QString& fileName, quint64 size);
    void peerDecision(bool accepted, const QString& reason);
    void peerChanged(const QString& name, const QString& id);
    void progressChanged(bool sending, const QString& fileName, quint64 done, quint64 total);
    void transferFinished(bool sending, bool ok, const QString& message);

private slots:
    void onTick();

private:
    BleCentral* ble_ = nullptr;
    class QTimer* timer_ = nullptr;
    struct CoreHolder;
    CoreHolder* core_ = nullptr;
};
