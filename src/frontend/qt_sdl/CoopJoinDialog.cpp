#include "CoopJoinDialog.h"

#include <cstring>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QListWidget>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QDialogButtonBox>
#include <QUdpSocket>
#include <QNetworkInterface>
#include <QTimer>

#include "../../coop/CoopProtocol.h"

using namespace Coop;

CoopJoinDialog::CoopJoinDialog(const QString& lastHost, QWidget* parent) : QDialog(parent)
{
    setWindowTitle("PMD Coop beitreten");
    setMinimumWidth(420);

    auto* lay = new QVBoxLayout(this);
    lay->addWidget(new QLabel("Gefundene Hosts im Netzwerk:", this));

    list = new QListWidget(this);
    list->setMinimumHeight(140);
    lay->addWidget(list);

    status = new QLabel("Suche läuft...", this);
    lay->addWidget(status);

    auto* row = new QHBoxLayout();
    row->addWidget(new QLabel("Oder Adresse eingeben:", this));
    manual = new QLineEdit(lastHost, this);
    manual->setPlaceholderText("z. B. 192.168.178.23 oder 127.0.0.1:7064");
    row->addWidget(manual, 1);
    lay->addLayout(row);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, this);
    connectBtn = buttons->addButton("Verbinden", QDialogButtonBox::AcceptRole);
    lay->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &CoopJoinDialog::onAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(list, &QListWidget::itemSelectionChanged, this, &CoopJoinDialog::onSelectionChanged);
    connect(list, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) { onAccept(); });

    sock = new QUdpSocket(this);
    sock->bind(QHostAddress::AnyIPv4, 0);
    connect(sock, &QUdpSocket::readyRead, this, &CoopJoinDialog::readReplies);

    timer = new QTimer(this);
    connect(timer, &QTimer::timeout, this, &CoopJoinDialog::sendProbe);
    timer->start(1000);
    sendProbe();
}

void CoopJoinDialog::sendProbe()
{
    QByteArray probe(DiscoveryRequest, (int)strlen(DiscoveryRequest));
    sock->writeDatagram(probe, QHostAddress::Broadcast, DiscoveryPort);
    sock->writeDatagram(probe, QHostAddress::LocalHost, DiscoveryPort);
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces())
    {
        if (!(iface.flags() & QNetworkInterface::IsUp) || !(iface.flags() & QNetworkInterface::CanBroadcast)) continue;
        for (const QNetworkAddressEntry& e : iface.addressEntries())
            if (e.ip().protocol() == QAbstractSocket::IPv4Protocol && !e.broadcast().isNull())
                sock->writeDatagram(probe, e.broadcast(), DiscoveryPort);
    }
}

void CoopJoinDialog::readReplies()
{
    while (sock->hasPendingDatagrams())
    {
        QByteArray data;
        data.resize((int)sock->pendingDatagramSize());
        QHostAddress from;
        sock->readDatagram(data.data(), data.size(), &from);
        if (data.size() < (int)sizeof(DiscoveryReply)) continue;
        DiscoveryReply r;
        memcpy(&r, data.constData(), sizeof(r));
        if (memcmp(r.magic, DiscoveryMagic, sizeof(r.magic)) != 0) continue;

        QString ip = QHostAddress(from.toIPv4Address()).toString();
        QString addr = r.port == DefaultPort ? ip : QString("%1:%2").arg(ip).arg(r.port);
        QString name = QString::fromUtf8(r.name, (int)strnlen(r.name, sizeof(r.name)));
        if (name.isEmpty()) name = "Host";
        QString text = QString("%1  -  %2   (%3/%4 Spieler)").arg(name, addr).arg(r.players).arg(r.maxPlayers);
        if (!r.coopRom) text += "   [noch kein Coop-ROM geladen]";
        if (r.version != ProtocolVersion) text += "   [andere Version!]";

        // one entry per host (it answers once per network interface): keep the most useful address
        auto rank = [](const QHostAddress& a) {
            if (a.isLoopback()) return 1;
            QString s = a.toString();
            if (s.startsWith("192.168.") || s.startsWith("10.")) return 3;  // typical home LAN
            return 2;                                                       // other (VPN, virtual switches)
        };
        int myRank = rank(from);
        bool found = false;
        for (int i = 0; i < list->count(); i++)
        {
            QListWidgetItem* it = list->item(i);
            if (it->data(Qt::UserRole + 1).toUInt() != r.hostId) continue;
            found = true;
            if (myRank >= it->data(Qt::UserRole + 2).toInt())
            {
                it->setText(text);
                it->setData(Qt::UserRole, addr);
                it->setData(Qt::UserRole + 2, myRank);
                if (it == list->currentItem()) manual->setText(addr);
            }
            break;
        }
        if (!found)
        {
            auto* it = new QListWidgetItem(text, list);
            it->setData(Qt::UserRole, addr);
            it->setData(Qt::UserRole + 1, r.hostId);
            it->setData(Qt::UserRole + 2, myRank);
            if (list->count() == 1) list->setCurrentRow(0);
        }
        status->setText(QString("%1 Host(s) gefunden").arg(list->count()));
    }
}

void CoopJoinDialog::onSelectionChanged()
{
    QListWidgetItem* it = list->currentItem();
    if (it) manual->setText(it->data(Qt::UserRole).toString());
}

void CoopJoinDialog::onAccept()
{
    chosen = manual->text().trimmed();
    if (chosen.isEmpty()) return;
    accept();
}
