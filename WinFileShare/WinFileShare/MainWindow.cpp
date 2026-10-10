#include "MainWindow.h"

#include "ble/BleCentral.h"
#include "protocol/Ids.h"
#include "session/LinkSession.h"

#include <QDateTime>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

namespace {
constexpr int kP2PConnected = static_cast<int>(p2p::LinkState::P2PConnected);
}

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    buildUi();
    setAcceptDrops(true);

    ble_ = new BleCentral();
    session_ = new LinkSession(ble_);
    bleThread_ = new QThread(this);
    linkThread_ = new QThread(this);
    ble_->moveToThread(bleThread_);
    session_->moveToThread(linkThread_);
    connect(bleThread_, &QThread::started, ble_, &BleCentral::initOnThread, Qt::DirectConnection);
    connect(linkThread_, &QThread::started, session_, &LinkSession::init, Qt::DirectConnection);

    connect(ble_, &BleCentral::logMessage, this, [this](const QString& line) { log_->appendPlainText(line); });
    connect(ble_, &BleCentral::errorOccurred, this, [this](const QString& message) {
        bleStateLabel_->setText("蓝牙：" + message);
        QMessageBox::warning(this, "蓝牙", message);
    });
    connect(ble_, &BleCentral::devicesChanged, this, &MainWindow::refreshDevices);
    connect(ble_, &BleCentral::transportUp, session_, &LinkSession::onTransportUp);
    connect(ble_, &BleCentral::transportDown, session_, &LinkSession::onTransportDown);
    connect(ble_, &BleCentral::packetReceived, session_, &LinkSession::onPacket);
    connect(ble_, &BleCentral::attPayloadChanged, session_, &LinkSession::onAttPayload);
    connect(ble_, &BleCentral::transportUp, this, [this](int) {
        const auto rows = ble_->snapshot();
        for (const auto& row : rows) {
            if (!deviceTable_->selectedItems().isEmpty() &&
                deviceTable_->item(deviceTable_->currentRow(), 0)->data(Qt::UserRole).toULongLong() == row.address) {
                connectedAddress_ = row.address;
            }
        }
    });

    connect(session_, &LinkSession::logMessage, this, [this](const QString& line) { log_->appendPlainText(line); });
    connect(session_, &LinkSession::stateChanged, this, &MainWindow::setLinkState);
    connect(session_, &LinkSession::incomingOffer, this, &MainWindow::showIncoming);
    connect(session_, &LinkSession::progressChanged, this, &MainWindow::updateProgress);
    connect(session_, &LinkSession::transferFinished, this, [this](bool, bool ok, const QString& message) {
        transferLabel_->setText(message);
        if (ok) progress_->setValue(100);
        log_->appendPlainText(message);
    });
    connect(session_, &LinkSession::peerChanged, this, [this](const QString& name, const QString& id) {
        deviceNameLabel_->setText("对端：" + name + " / " + id);
        refreshDevices();
        if (connectedAddress_ == 0) return;
        for (int i = 0; i < deviceTable_->rowCount(); ++i) {
            if (deviceTable_->item(i, 0)->data(Qt::UserRole).toULongLong() == connectedAddress_) {
                deviceTable_->item(i, 0)->setText(name);
                deviceTable_->item(i, 1)->setText(id);
            }
        }
    });
    connect(session_, &LinkSession::peerDecision, this, [this](bool accepted, const QString& reason) {
        transferLabel_->setText(accepted ? "对方已接受，开始发送" : ("对方未接受：" + reason));
    });

    connect(discoverButton_, &QPushButton::clicked, ble_, &BleCentral::startScan);
    connect(stopButton_, &QPushButton::clicked, ble_, &BleCentral::stopScan);
    connect(connectButton_, &QPushButton::clicked, this, [this] {
        const int row = deviceTable_->currentRow();
        if (row < 0) {
            QMessageBox::information(this, "连接手机", "请先在列表中选择一台手机");
            return;
        }
        connectedAddress_ = deviceTable_->item(row, 0)->data(Qt::UserRole).toULongLong();
        QMetaObject::invokeMethod(ble_, [this] { ble_->connectTo(connectedAddress_); }, Qt::QueuedConnection);
    });
    connect(disconnectButton_, &QPushButton::clicked, ble_, &BleCentral::disconnectDevice);
    connect(sendButton_, &QPushButton::clicked, this, [this] {
        const QStringList files = QFileDialog::getOpenFileNames(this, "选择要发送的文件");
        queueFiles(files);
    });
    connect(cancelSendButton_, &QPushButton::clicked, session_, &LinkSession::cancelSend);
    connect(cancelRecvButton_, &QPushButton::clicked, session_, &LinkSession::cancelReceive);

    bleThread_->start();
    linkThread_->start();
}

MainWindow::~MainWindow() {
    if (session_) QMetaObject::invokeMethod(session_, "shutdown", Qt::BlockingQueuedConnection);
    if (linkThread_) {
        linkThread_->quit();
        linkThread_->wait(3000);
    }
    if (bleThread_) {
        bleThread_->quit();
        bleThread_->wait(3000);
    }
    delete session_;
    delete ble_;
    session_ = nullptr;
    ble_ = nullptr;
}

void MainWindow::buildUi() {
    setWindowTitle("WinFileShare");
    resize(980, 720);
    auto* central = new QWidget(this);
    auto* root = new QVBoxLayout(central);

    auto* title = new QLabel("WinFileShare  未加密学习原型，请不要传输敏感文件", central);
    auto* header = new QHBoxLayout();
    deviceNameLabel_ = new QLabel("本机 Windows", central);
    bleStateLabel_ = new QLabel("蓝牙：空闲", central);
    p2pStateLabel_ = new QLabel("P2P：未连接", central);
    header->addWidget(title);
    header->addStretch();
    header->addWidget(deviceNameLabel_);
    header->addWidget(bleStateLabel_);
    header->addWidget(p2pStateLabel_);
    root->addLayout(header);

    auto* buttons = new QHBoxLayout();
    discoverButton_ = new QPushButton("发现设备", central);
    stopButton_ = new QPushButton("停止发现", central);
    connectButton_ = new QPushButton("连接手机", central);
    disconnectButton_ = new QPushButton("断开连接", central);
    sendButton_ = new QPushButton("选择文件发送", central);
    sendButton_->setEnabled(false);
    buttons->addWidget(discoverButton_);
    buttons->addWidget(stopButton_);
    buttons->addWidget(connectButton_);
    buttons->addWidget(disconnectButton_);
    buttons->addWidget(sendButton_);
    buttons->addStretch();
    root->addLayout(buttons);

    deviceTable_ = new QTableWidget(0, 6, central);
    deviceTable_->setHorizontalHeaderLabels({"设备名称", "设备标识", "信号", "最近发现", "蓝牙", "P2P"});
    deviceTable_->horizontalHeader()->setStretchLastSection(true);
    deviceTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    deviceTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    deviceTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    root->addWidget(deviceTable_, 1);

    dropLabel_ = new QLabel("将文件拖到这里发送到手机", central);
    dropLabel_->setAlignment(Qt::AlignCenter);
    dropLabel_->setMinimumHeight(72);
    dropLabel_->setFrameStyle(QFrame::Box);
    root->addWidget(dropLabel_);

    transferLabel_ = new QLabel("当前没有传输", central);
    speedLabel_ = new QLabel(central);
    progress_ = new QProgressBar(central);
    progress_->setRange(0, 100);
    auto* transferRow = new QHBoxLayout();
    cancelSendButton_ = new QPushButton("取消发送", central);
    cancelRecvButton_ = new QPushButton("取消接收", central);
    transferRow->addWidget(transferLabel_, 1);
    transferRow->addWidget(speedLabel_);
    transferRow->addWidget(cancelSendButton_);
    transferRow->addWidget(cancelRecvButton_);
    root->addLayout(transferRow);
    root->addWidget(progress_);

    log_ = new QPlainTextEdit(central);
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(400);
    log_->setPlaceholderText("日志");
    root->addWidget(log_, 1);
    setCentralWidget(central);
}

void MainWindow::refreshDevices() {
    const auto rows = ble_->snapshot();
    const quint64 selected = deviceTable_->currentRow() >= 0 && deviceTable_->item(deviceTable_->currentRow(), 0)
                                 ? deviceTable_->item(deviceTable_->currentRow(), 0)->data(Qt::UserRole).toULongLong()
                                 : 0;
    deviceTable_->setRowCount(rows.size());
    int select = -1;
    for (int i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        auto* name = new QTableWidgetItem(row.name);
        name->setData(Qt::UserRole, row.address);
        deviceTable_->setItem(i, 0, name);
        deviceTable_->setItem(i, 1, new QTableWidgetItem(row.deviceId));
        deviceTable_->setItem(i, 2, new QTableWidgetItem(QString::number(row.rssi) + " dBm"));
        deviceTable_->setItem(i, 3, new QTableWidgetItem(row.lastSeen));
        const bool connected = row.address == connectedAddress_ && linkState_ != static_cast<int>(p2p::LinkState::Disconnected) &&
                               linkState_ != static_cast<int>(p2p::LinkState::Idle);
        QString ble = row.stale ? "已超时" : "已发现";
        QString p2p = "未连接";
        if (connected) {
            ble = p2pReady_ || linkState_ >= static_cast<int>(p2p::LinkState::BleConnected) ? "已连接" : "正在连接";
            if (linkState_ == static_cast<int>(p2p::LinkState::IceFailed)) p2p = "直连失败";
            else if (p2pReady_) p2p = "已连接";
            else if (linkState_ == static_cast<int>(p2p::LinkState::IceGathering) ||
                     linkState_ == static_cast<int>(p2p::LinkState::IceConnecting))
                p2p = "连接中";
        }
        deviceTable_->setItem(i, 4, new QTableWidgetItem(ble));
        deviceTable_->setItem(i, 5, new QTableWidgetItem(p2p));
        if (row.address == selected) select = i;
    }
    if (select >= 0) deviceTable_->selectRow(select);
}

void MainWindow::setLinkState(int state, const QString& detail) {
    linkState_ = state;
    const auto link = static_cast<p2p::LinkState>(state);
    QString ble = "蓝牙：空闲";
    QString p2p = "P2P：未连接";
    if (link == p2p::LinkState::BleScanning) ble = "蓝牙：正在扫描";
    else if (link == p2p::LinkState::BleConnecting) ble = "蓝牙：正在连接";
    else if (link == p2p::LinkState::BleConnected || link == p2p::LinkState::IceGathering ||
             link == p2p::LinkState::IceConnecting || link == p2p::LinkState::P2PConnected ||
             link == p2p::LinkState::IceFailed) {
        ble = "蓝牙：已连接";
    } else if (link == p2p::LinkState::Disconnecting) ble = "蓝牙：正在断开";
    else if (link == p2p::LinkState::Disconnected) {
        ble = "蓝牙：已断开";
        connectedAddress_ = 0;
    }
    p2pReady_ = link == p2p::LinkState::P2PConnected;
    if (p2pReady_) p2p = "P2P：已连接";
    else if (link == p2p::LinkState::IceGathering || link == p2p::LinkState::IceConnecting) p2p = "P2P：正在连接";
    else if (link == p2p::LinkState::IceFailed) p2p = "P2P：直连失败";
    bleStateLabel_->setText(ble);
    p2pStateLabel_->setText(p2p);
    sendButton_->setEnabled(p2pReady_);
    if (!detail.isEmpty()) {
        log_->appendPlainText(detail);
        if (link == p2p::LinkState::IceFailed) p2pStateLabel_->setToolTip(detail);
    }
    refreshDevices();
}

void MainWindow::queueFiles(const QStringList& paths) {
    if (paths.isEmpty()) return;
    if (!p2pReady_) {
        QMessageBox::information(this, "发送文件", "P2P 尚未连接。文件会保留在队列中，连接成功后才开始发送。");
    }
    for (const QString& path : paths) {
        QMetaObject::invokeMethod(session_, [this, path] { session_->sendFile(path); }, Qt::QueuedConnection);
    }
}

void MainWindow::showIncoming(const QString& name, quint64 size) {
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle("接收文件");
    box.setText("手机正在向电脑发送文件");
    box.setInformativeText(QString("文件名称：%1\n文件大小：%2").arg(name, formatBytes(size)));
    auto* accept = box.addButton("接收", QMessageBox::AcceptRole);
    box.addButton("拒绝", QMessageBox::RejectRole);
    box.exec();
    if (box.clickedButton() != accept) {
        QMetaObject::invokeMethod(session_, [this] { session_->rejectFile("用户拒绝接收"); }, Qt::QueuedConnection);
        return;
    }
    const QString path = QFileDialog::getSaveFileName(this, "保存接收的文件", name);
    if (path.isEmpty()) {
        QMetaObject::invokeMethod(session_, [this] { session_->rejectFile("用户取消保存"); }, Qt::QueuedConnection);
        return;
    }
    progressBytes_ = 0;
    progressMs_ = 0;
    QMetaObject::invokeMethod(session_, [this, path] { session_->acceptFile(path); }, Qt::QueuedConnection);
}

void MainWindow::updateProgress(bool sending, const QString& name, quint64 done, quint64 total) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QString speed = "速度计算中";
    QString eta;
    if (progressMs_ > 0 && now > progressMs_ && done >= progressBytes_) {
        const double dt = (now - progressMs_) / 1000.0;
        if (dt > 0.05) {
            const double bps = (done - progressBytes_) / dt;
            speed = QString("%1/s").arg(formatBytes(static_cast<quint64>(bps)));
            if (bps > 1.0 && total > done) {
                const qint64 seconds = static_cast<qint64>((total - done) / bps);
                eta = QString("，剩余约 %1 秒").arg(seconds);
            }
        }
    }
    progressBytes_ = done;
    progressMs_ = now;
    transferLabel_->setText(QString("%1 %2  %3 / %4")
                                .arg(sending ? "正在发送" : "正在接收", name, formatBytes(done), formatBytes(total)));
    speedLabel_->setText(speed + eta);
    progress_->setValue(total == 0 ? 100 : static_cast<int>((done * 100) / total));
}

QString MainWindow::formatBytes(quint64 bytes) const {
    if (bytes < 1024) return QString::number(bytes) + " B";
    if (bytes < 1024 * 1024) return QString::number(bytes / 1024.0, 'f', 1) + " KB";
    if (bytes < 1024ull * 1024 * 1024) return QString::number(bytes / 1024.0 / 1024.0, 'f', 1) + " MB";
    return QString::number(bytes / 1024.0 / 1024.0 / 1024.0, 'f', 2) + " GB";
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void MainWindow::dragMoveEvent(QDragMoveEvent* event) {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event) {
    QStringList files;
    for (const QUrl& url : event->mimeData()->urls()) {
        if (!url.isLocalFile()) continue;
        const QFileInfo info(url.toLocalFile());
        if (info.isFile()) files.push_back(info.absoluteFilePath());
    }
    if (files.isEmpty()) {
        QMessageBox::information(this, "发送文件", "只接受本机上的普通文件");
        return;
    }
    queueFiles(files);
    event->acceptProposedAction();
}
