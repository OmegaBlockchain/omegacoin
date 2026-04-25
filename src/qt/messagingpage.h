// Copyright (c) 2017-2024 The Particl Core developers
// Copyright (c) 2026 The Omega Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef OMEGA_QT_MESSAGINGPAGE_H
#define OMEGA_QT_MESSAGINGPAGE_H

#include <amount.h>
#include <smsg/smessage.h>

#include <QWidget>
#include <QMenu>
#include <QTimer>

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
    void onGenerateKeyClicked();

    // Inbox context menu actions
    void showInboxMessage();
    void replyToSelected();
    void copyFromAddress();
    void copyToAddress();
    void copyMessageId();
    void markRead();
    void markUnread();
    void deleteSelectedInbox();
    void purgeSelectedInbox();

    // Outbox context menu actions
    void showOutboxMessage();
    void copyOutboxFromAddress();
    void copyOutboxToAddress();
    void copyOutboxMessageId();
    void deleteSelectedOutbox();

    // Keys context menu actions
    void toggleReceive();
    void toggleAnon();
    void copyKeyAddress();
    void copyKeyPublicKey();
    void sendMessageToSelected();
    void deleteContact();

    // Trollbox actions
    void onTrollboxSendClicked();
    void updateTrollboxList();
    void updateTrollboxFromAddresses();
    void onTrollboxCooldownTick();
    void showTrollboxContextMenu(const QPoint& point);
    void copyTrollboxMessage();
    void copyTrollboxSender();
    void muteTrollboxSender();

private:
    std::unique_ptr<Ui::MessagingPage> ui;
    ClientModel* clientModel;

    QMenu* inboxContextMenu;
    QMenu* outboxContextMenu;
    QMenu* keysContextMenu;
    QMenu* trollboxContextMenu;
    QAction* sendMessageAction;
    QAction* deleteContactAction;

    QTimer* updateTimer;
    QTimer* trollboxCooldownTimer;
    std::atomic<bool> fInboxChanged{false};
    std::atomic<bool> fOutboxChanged{false};
    std::atomic<bool> fTrollboxChanged{false};

    int nTrollboxCooldown;
    std::set<std::string> trollboxMuteList;
    QString trollboxSelectedSender;
    QString trollboxSelectedMessage;

    boost::signals2::connection m_smsg_inbox_conn;
    boost::signals2::connection m_smsg_outbox_conn;
    boost::signals2::connection m_smsg_wallet_unlocked_conn;
    boost::signals2::connection m_smsg_trollbox_conn;

    void setupInboxTab();
    void setupOutboxTab();
    void setupComposeTab();
    void setupKeysTab();
    void setupTrollboxTab();
    void connectSignals();
    void disconnectSignals();
    void updateFromAddresses();
    void updateFeeEstimate();
    void updateSmsgEnabledState();
    void filterInbox(const QString& text);
    void filterOutbox(const QString& text);
    void filterKeys(const QString& text);

    std::thread m_scanThread;
    std::atomic<bool> m_fScanning{false};
    CAmount m_lastFeeEstimate{0};

    int64_t m_trollboxLastTimestamp{0};
    size_t  m_trollboxLastCount{0};

    // Decrypt cache — keyed by raw 30-byte DB key string.
    // Avoids re-running ECDH on every 3 s timer tick.
    struct DecryptedRow {
        std::array<uint8_t, 30> chKey{};
        int64_t  timeReceived{0};
        int64_t  timeSent{0};
        QString  sFrom;
        QString  sTo;
        QString  sText;
        QString  sMsgId;
        uint8_t  status{0};
        uint32_t nDaysRetention{2};
        bool     fPaid{false};
        bool     fUnread{false};
        bool     fDecryptFailed{false};
    };
    struct TrollboxCached {
        std::array<uint8_t, 30> chKey{};
        int64_t time{0};
        QString from;
        QString text;
        bool    fPaid{false};
        bool    fDecryptFailed{false};
    };
    std::unordered_map<std::string, DecryptedRow>   m_inboxCache;
    std::unordered_map<std::string, DecryptedRow>   m_outboxCache;
    std::unordered_map<std::string, TrollboxCached> m_trollboxCache;

    std::mutex                       m_pendingInboxMutex;
    std::vector<smsg::SmsgGuiRow>    m_pendingInbox;
    std::mutex                       m_pendingTrollboxMutex;
    std::vector<smsg::SmsgGuiRow>    m_pendingTrollbox;

    // Keystore snapshot — rebuilt once per keystore mutation, consumed by all three
    // address-list builders to avoid repeated EncodeDestination / HexStr passes.
    struct KeyRowVM {
        QString sAddr;
        QString sLabel;
        QString sPubKey;
        bool    fContact{false};
        bool    fRecv{false};
        bool    fAnon{false};
        bool    fHasPrivKey{false};
        bool    fIsWalletAddr{false};
    };
    std::vector<KeyRowVM>  m_keystoreSnapshot;
    std::atomic<bool>      m_keystoreDirty{true};

    void buildKeystoreSnapshot();
    void invalidateKeystoreSnapshot() { m_keystoreDirty = true; }
};

#endif // OMEGA_QT_MESSAGINGPAGE_H
