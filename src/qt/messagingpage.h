// Copyright (c) 2026 The Omega Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef OMEGA_QT_MESSAGINGPAGE_H
#define OMEGA_QT_MESSAGINGPAGE_H

#include <QWidget>
#include <QMenu>
#include <QTimer>

#include <memory>

#include <boost/signals2/signal.hpp>

class ClientModel;

namespace Ui {
    class MessagingPage;
}

class MessagingPage : public QWidget
{
    Q_OBJECT

public:
    explicit MessagingPage(QWidget* parent = nullptr);
    ~MessagingPage() override;

    void setClientModel(ClientModel* model);

private Q_SLOTS:
    void updateInboxList();
    void updateOutboxList();
    void updateKeysList();
    void updateScheduled();

    void showInboxContextMenu(const QPoint& point);
    void showOutboxContextMenu(const QPoint& point);
    void showKeysContextMenu(const QPoint& point);

    void onSendClicked();
    void onPaidToggled(bool checked);
    void onMessageTextChanged();
    void onRetentionChanged(int value);
    void onTabChanged(int index);
    void onEnableSmsgClicked();

    void onScanChainClicked();
    void onAddAddressClicked();

    // Inbox context menu actions
    void copyFromAddress();
    void copyToAddress();
    void copyMessageId();
    void markRead();
    void markUnread();
    void deleteSelectedInbox();
    void purgeSelectedInbox();

    // Outbox context menu actions
    void copyOutboxFromAddress();
    void copyOutboxToAddress();
    void copyOutboxMessageId();
    void deleteSelectedOutbox();

    // Keys context menu actions
    void toggleReceive();
    void toggleAnon();
    void copyKeyAddress();

private:
    std::unique_ptr<Ui::MessagingPage> ui;
    ClientModel* clientModel;

    QMenu* inboxContextMenu;
    QMenu* outboxContextMenu;
    QMenu* keysContextMenu;

    QTimer* updateTimer;
    bool fInboxChanged;
    bool fOutboxChanged;

    boost::signals2::connection m_smsg_inbox_conn;
    boost::signals2::connection m_smsg_outbox_conn;
    boost::signals2::connection m_smsg_wallet_unlocked_conn;

    void setupInboxTab();
    void setupOutboxTab();
    void setupComposeTab();
    void setupKeysTab();
    void connectSignals();
    void disconnectSignals();
    void updateFromAddresses();
    void updateFeeEstimate();
    void updateSmsgEnabledState();
    void filterInbox(const QString& text);
    void filterOutbox(const QString& text);
    void filterKeys(const QString& text);
};

#endif // OMEGA_QT_MESSAGINGPAGE_H
