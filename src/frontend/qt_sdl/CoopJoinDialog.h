// "PMD Coop: Beitreten..." dialog: lists hosts found on the local network (UDP broadcast on
// Coop::DiscoveryPort) and allows entering an address manually.
#pragma once

#include <QDialog>
#include <QString>

class QListWidget;
class QLineEdit;
class QLabel;
class QPushButton;
class QUdpSocket;
class QTimer;

class CoopJoinDialog : public QDialog
{
    Q_OBJECT

public:
    explicit CoopJoinDialog(const QString& lastHost, QWidget* parent = nullptr);

    // "ip" or "ip:port" chosen by the user
    QString address() const { return chosen; }

private slots:
    void sendProbe();
    void readReplies();
    void onSelectionChanged();
    void onAccept();

private:
    QListWidget* list;
    QLineEdit* manual;
    QLabel* status;
    QPushButton* connectBtn;
    QUdpSocket* sock;
    QTimer* timer;
    QString chosen;
};
