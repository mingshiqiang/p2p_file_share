#pragma once

#include <QMainWindow>
#include <QString>

class BleCentral;
class LinkSession;
class QLabel;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QThread;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void buildUi();
    void refreshDevices();
    void setLinkState(int state, const QString& detail);
    void queueFiles(const QStringList& paths);
    void showIncoming(const QString& name, quint64 size);
    void updateProgress(bool sending, const QString& name, quint64 done, quint64 total);
    QString formatBytes(quint64 bytes) const;

    BleCentral* ble_ = nullptr;
    LinkSession* session_ = nullptr;
    QThread* bleThread_ = nullptr;
    QThread* linkThread_ = nullptr;

    QLabel* deviceNameLabel_ = nullptr;
    QLabel* bleStateLabel_ = nullptr;
    QLabel* p2pStateLabel_ = nullptr;
    QLabel* dropLabel_ = nullptr;
    QLabel* transferLabel_ = nullptr;
    QLabel* speedLabel_ = nullptr;
    QPushButton* discoverButton_ = nullptr;
    QPushButton* stopButton_ = nullptr;
    QPushButton* connectButton_ = nullptr;
    QPushButton* disconnectButton_ = nullptr;
    QPushButton* sendButton_ = nullptr;
    QPushButton* cancelSendButton_ = nullptr;
    QPushButton* cancelRecvButton_ = nullptr;
    QTableWidget* deviceTable_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QPlainTextEdit* log_ = nullptr;

    int linkState_ = 0;
    quint64 connectedAddress_ = 0;
    quint64 progressBytes_ = 0;
    qint64 progressMs_ = 0;
    bool p2pReady_ = false;
};
