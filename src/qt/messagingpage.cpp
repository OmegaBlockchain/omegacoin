// Copyright (c) 2017-2024 The Particl Core developers
// Copyright (c) 2026 The Omega Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/forms/ui_messagingpage.h>
#include <qt/messagingpage.h>

#include <key_io.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <smsg/smessage.h>
#include <smsg/db.h>
#include <util/string.h>
#ifdef ENABLE_WALLET
#include <wallet/wallet.h>
#endif

#include <QApplication>
#include <QClipboard>
#include <QDateTime>
#include <QInputDialog>
#include <QMessageBox>
#include <QMetaObject>
#include <QTextCursor>

// Inbox column indices
enum InboxColumns {
    INBOX_COL_READ = 0,
    INBOX_COL_DATE_RECV,
    INBOX_COL_DATE_SENT,
    INBOX_COL_FROM,
    INBOX_COL_TO,
    INBOX_COL_PAID,
    INBOX_COL_RETENTION,
    INBOX_COL_TEXT,
    INBOX_COL_MSGID, // hidden
    INBOX_COL_COUNT
};

// Outbox column indices
enum OutboxColumns {
    OUTBOX_COL_DATE_SENT = 0,
    OUTBOX_COL_FROM,
    OUTBOX_COL_TO,
    OUTBOX_COL_PAID,
    OUTBOX_COL_RETENTION,
    OUTBOX_COL_TEXT,
    OUTBOX_COL_MSGID, // hidden
    OUTBOX_COL_COUNT
};

// Keys column indices
enum KeysColumns {
    KEYS_COL_ADDRESS = 0,
    KEYS_COL_LABEL,
    KEYS_COL_RECEIVE,
    KEYS_COL_ANON,
    KEYS_COL_SOURCE,
    KEYS_COL_COUNT
};

MessagingPage::MessagingPage(QWidget* parent) :
    QWidget(parent),
    ui(new Ui::MessagingPage),
    clientModel(nullptr),
    inboxContextMenu(nullptr),
    outboxContextMenu(nullptr),
    keysContextMenu(nullptr),
    trollboxContextMenu(nullptr),
    sendMessageAction(nullptr),
    updateTimer(nullptr),
    trollboxCooldownTimer(nullptr),
    nTrollboxCooldown(0)
{
    ui->setupUi(this);

    setupInboxTab();
    setupOutboxTab();
    setupComposeTab();
    setupKeysTab();
    setupTrollboxTab();

    // Update timer — 3-second cooldown matching MasternodeList pattern
    updateTimer = new QTimer(this);
    updateTimer->setInterval(3000);
    connect(updateTimer, &QTimer::timeout, this, &MessagingPage::updateScheduled);
    updateTimer->start();

    // Trollbox cooldown timer — 1-second ticks
    trollboxCooldownTimer = new QTimer(this);
    trollboxCooldownTimer->setInterval(1000);
    connect(trollboxCooldownTimer, &QTimer::timeout, this, &MessagingPage::onTrollboxCooldownTick);

    // Tab change handler
    connect(ui->tabWidget, &QTabWidget::currentChanged, this, &MessagingPage::onTabChanged);

    // Enable SMSG button
    connect(ui->enableSmsgButton, &QPushButton::clicked, this, &MessagingPage::onEnableSmsgClicked);
}

MessagingPage::~MessagingPage()
{
    disconnectSignals();
    if (m_scanThread.joinable())
        m_scanThread.join();
}

void MessagingPage::setupInboxTab()
{
    QTableWidget* table = ui->inboxTable;
    table->setColumnCount(INBOX_COL_COUNT);
    table->setHorizontalHeaderLabels({
        tr("Read"), tr("Received"), tr("Sent"), tr("From"), tr("To"),
        tr("Paid"), tr("Retention"), tr("Text"), tr("MsgID")
    });
    table->setColumnHidden(INBOX_COL_MSGID, true);
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(INBOX_COL_TEXT, QHeaderView::Stretch);
    table->setColumnWidth(INBOX_COL_READ, 40);
    table->setColumnWidth(INBOX_COL_DATE_RECV, 140);
    table->setColumnWidth(INBOX_COL_DATE_SENT, 140);
    table->setColumnWidth(INBOX_COL_FROM, 160);
    table->setColumnWidth(INBOX_COL_TO, 160);
    table->setColumnWidth(INBOX_COL_PAID, 40);
    table->setColumnWidth(INBOX_COL_RETENTION, 60);
    table->verticalHeader()->setVisible(false);

    table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table, &QTableWidget::customContextMenuRequested, this, &MessagingPage::showInboxContextMenu);
    connect(table, &QTableWidget::cellDoubleClicked, this, [this](int, int){ showInboxMessage(); });

    // Context menu
    inboxContextMenu = new QMenu(this);
    inboxContextMenu->addAction(tr("Show Message"), this, &MessagingPage::showInboxMessage);
    inboxContextMenu->addSeparator();
    inboxContextMenu->addAction(tr("Copy From Address"), this, &MessagingPage::copyFromAddress);
    inboxContextMenu->addAction(tr("Copy To Address"), this, &MessagingPage::copyToAddress);
    inboxContextMenu->addAction(tr("Copy Message ID"), this, &MessagingPage::copyMessageId);
    inboxContextMenu->addSeparator();
    inboxContextMenu->addAction(tr("Mark as Read"), this, &MessagingPage::markRead);
    inboxContextMenu->addAction(tr("Mark as Unread"), this, &MessagingPage::markUnread);
    inboxContextMenu->addSeparator();
    inboxContextMenu->addAction(tr("Reply"), this, &MessagingPage::replyToSelected);
    inboxContextMenu->addSeparator();
    inboxContextMenu->addAction(tr("Delete"), this, &MessagingPage::deleteSelectedInbox);
    inboxContextMenu->addAction(tr("Purge from Network"), this, &MessagingPage::purgeSelectedInbox);

    connect(ui->inboxFilterLineEdit, &QLineEdit::textChanged, this, &MessagingPage::filterInbox);
}

void MessagingPage::setupOutboxTab()
{
    QTableWidget* table = ui->outboxTable;
    table->setColumnCount(OUTBOX_COL_COUNT);
    table->setHorizontalHeaderLabels({
        tr("Sent"), tr("From"), tr("To"),
        tr("Paid"), tr("Retention"), tr("Text"), tr("MsgID")
    });
    table->setColumnHidden(OUTBOX_COL_MSGID, true);
    table->horizontalHeader()->setStretchLastSection(true);
    table->horizontalHeader()->setSectionResizeMode(OUTBOX_COL_TEXT, QHeaderView::Stretch);
    table->setColumnWidth(OUTBOX_COL_DATE_SENT, 140);
    table->setColumnWidth(OUTBOX_COL_FROM, 160);
    table->setColumnWidth(OUTBOX_COL_TO, 160);
    table->setColumnWidth(OUTBOX_COL_PAID, 40);
    table->setColumnWidth(OUTBOX_COL_RETENTION, 60);
    table->verticalHeader()->setVisible(false);

    table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table, &QTableWidget::customContextMenuRequested, this, &MessagingPage::showOutboxContextMenu);
    connect(table, &QTableWidget::cellDoubleClicked, this, [this](int, int){ showOutboxMessage(); });

    // Context menu
    outboxContextMenu = new QMenu(this);
    outboxContextMenu->addAction(tr("Show Message"), this, &MessagingPage::showOutboxMessage);
    outboxContextMenu->addSeparator();
    outboxContextMenu->addAction(tr("Copy From Address"), this, &MessagingPage::copyOutboxFromAddress);
    outboxContextMenu->addAction(tr("Copy To Address"), this, &MessagingPage::copyOutboxToAddress);
    outboxContextMenu->addAction(tr("Copy Message ID"), this, &MessagingPage::copyOutboxMessageId);
    outboxContextMenu->addSeparator();
    outboxContextMenu->addAction(tr("Delete"), this, &MessagingPage::deleteSelectedOutbox);

    connect(ui->outboxFilterLineEdit, &QLineEdit::textChanged, this, &MessagingPage::filterOutbox);
}

void MessagingPage::setupComposeTab()
{
    connect(ui->sendButton, &QPushButton::clicked, this, &MessagingPage::onSendClicked);
    connect(ui->paidCheckBox, &QCheckBox::toggled, this, &MessagingPage::onPaidToggled);
    connect(ui->messageEdit, &QPlainTextEdit::textChanged, this, &MessagingPage::onMessageTextChanged);
    connect(ui->retentionSpinBox, static_cast<void (QSpinBox::*)(int)>(&QSpinBox::valueChanged), this, &MessagingPage::onRetentionChanged);
    connect(ui->toAddressEdit, &QLineEdit::textChanged, this, [this](const QString&) { m_lastFeeEstimate = 0; });
    connect(ui->fromAddressCombo, &QComboBox::currentTextChanged, this, [this](const QString&) { m_lastFeeEstimate = 0; });
}

void MessagingPage::setupKeysTab()
{
    QTableWidget* table = ui->keysTable;
    table->setColumnCount(KEYS_COL_COUNT);
    table->setHorizontalHeaderLabels({
        tr("Address"), tr("Label"), tr("Receive"), tr("Anon"), tr("Type")
    });
    table->horizontalHeader()->setStretchLastSection(true);
    table->setColumnWidth(KEYS_COL_ADDRESS, 280);
    table->setColumnWidth(KEYS_COL_LABEL, 150);
    table->setColumnWidth(KEYS_COL_RECEIVE, 60);
    table->setColumnWidth(KEYS_COL_ANON, 60);
    table->verticalHeader()->setVisible(false);

    table->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(table, &QTableWidget::customContextMenuRequested, this, &MessagingPage::showKeysContextMenu);

    keysContextMenu = new QMenu(this);
    keysContextMenu->addAction(tr("Toggle Receive"), this, &MessagingPage::toggleReceive);
    keysContextMenu->addAction(tr("Toggle Anon"), this, &MessagingPage::toggleAnon);
    keysContextMenu->addSeparator();
    keysContextMenu->addAction(tr("Copy Address"), this, &MessagingPage::copyKeyAddress);
    keysContextMenu->addAction(tr("Copy Public Key"), this, &MessagingPage::copyKeyPublicKey);
    keysContextMenu->addSeparator();
    sendMessageAction = keysContextMenu->addAction(tr("Send Message"), this, &MessagingPage::sendMessageToSelected);
    keysContextMenu->addSeparator();
    deleteContactAction = keysContextMenu->addAction(tr("Delete Contact"), this, &MessagingPage::deleteContact);

    connect(ui->scanChainButton, &QPushButton::clicked, this, &MessagingPage::onScanChainClicked);
    connect(ui->addAddressButton, &QPushButton::clicked, this, &MessagingPage::onAddAddressClicked);
    connect(ui->generateKeyButton, &QPushButton::clicked, this, &MessagingPage::onGenerateKeyClicked);

    connect(ui->keysFilterLineEdit, &QLineEdit::textChanged, this, &MessagingPage::filterKeys);
}

void MessagingPage::setClientModel(ClientModel* model)
{
    this->clientModel = model;
    if (model) {
        connectSignals();
        updateSmsgEnabledState();
    }
}

void MessagingPage::connectSignals()
{
    m_smsg_inbox_conn = smsg::NotifySecMsgInboxChanged.connect(
        [this](smsg::SecMsgStored& /*inboxHdr*/) {
            fInboxChanged = true;
        });

    m_smsg_outbox_conn = smsg::NotifySecMsgOutboxChanged.connect(
        [this](smsg::SecMsgStored& /*outboxHdr*/) {
            fOutboxChanged = true;
        });

    m_smsg_wallet_unlocked_conn = smsg::NotifySecMsgWalletUnlocked.connect(
        [this]() {
            fInboxChanged = true;
            fOutboxChanged = true;
            fTrollboxChanged = true;
        });

    m_smsg_trollbox_conn = smsg::NotifySecMsgTrollboxChanged.connect(
        [this](smsg::SecMsgStored& /*trollboxHdr*/) {
            fTrollboxChanged = true;
        });
}

void MessagingPage::disconnectSignals()
{
    m_smsg_inbox_conn.disconnect();
    m_smsg_outbox_conn.disconnect();
    m_smsg_wallet_unlocked_conn.disconnect();
    m_smsg_trollbox_conn.disconnect();
}

void MessagingPage::updateSmsgEnabledState()
{
    bool fEnabled = smsg::fSecMsgEnabled;
    ui->tabWidget->setVisible(fEnabled);
    ui->disabledLabel->setVisible(!fEnabled);
    ui->enableSmsgButton->setVisible(!fEnabled);

    if (fEnabled) {
        updateFromAddresses();
        updateTrollboxFromAddresses();
        onTabChanged(ui->tabWidget->currentIndex());
    }
}

void MessagingPage::onEnableSmsgClicked()
{
    if (smsg::fSecMsgEnabled)
        return;

    // Find a wallet the same way the smsgenable RPC does — smsgModule.pwallet
    // is only set after Start(), so it is always null here.
    std::shared_ptr<CWallet> pwallet;
#ifdef ENABLE_WALLET
    auto vpwallets = GetWallets();
    if (!vpwallets.empty())
        pwallet = vpwallets[0];
#endif

    if (!pwallet) {
        QMessageBox::warning(this, tr("Messaging"), tr("No wallet loaded. Cannot enable secure messaging."));
        return;
    }

    if (!smsgModule.Enable(pwallet)) {
        QMessageBox::warning(this, tr("Messaging"), tr("Failed to enable secure messaging."));
        return;
    }

    updateSmsgEnabledState();
}

void MessagingPage::updateScheduled()
{
    if (!smsg::fSecMsgEnabled)
        return;

    if (fInboxChanged) {
        fInboxChanged = false;
        // Marshal to Qt thread
        QMetaObject::invokeMethod(this, "updateInboxList", Qt::QueuedConnection);
    }
    if (fOutboxChanged) {
        fOutboxChanged = false;
        QMetaObject::invokeMethod(this, "updateOutboxList", Qt::QueuedConnection);
    }
    if (fTrollboxChanged) {
        fTrollboxChanged = false;
        QMetaObject::invokeMethod(this, "updateTrollboxList", Qt::QueuedConnection);
    }
}

void MessagingPage::onTabChanged(int index)
{
    if (index == 0) {
        updateInboxList();
    } else if (index == 1) {
        updateOutboxList();
    } else if (index == 2) {
        updateFromAddresses();
    } else if (index == 3) {
        updateKeysList();
    } else if (index == 4) {
        updateTrollboxFromAddresses();
        updateTrollboxList();
    }
}

void MessagingPage::updateInboxList()
{
    if (!smsg::fSecMsgEnabled)
        return;

    // Phase 1: key-only scan; read full stored only for cache misses / prior failures.
    struct ToDecrypt {
        std::string keyStr;
        std::array<uint8_t, 30> chKey;
        smsg::SecMsgStored stored;
    };
    std::vector<ToDecrypt> toDecrypt;
    std::unordered_set<std::string> currentKeys;

    {
        LOCK(smsg::cs_smsgDB);
        smsg::SecMsgDB db;
        if (!db.Open("cr+"))
            return;

        uint8_t chKey[30];
        std::string sPrefix("im");
        leveldb::Iterator* it = db.pdb->NewIterator(leveldb::ReadOptions());
        while (db.NextSmesgKey(it, sPrefix, chKey)) {
            std::string ks(reinterpret_cast<const char*>(chKey), 30);
            currentKeys.insert(ks);

            auto cit = m_inboxCache.find(ks);
            if (cit != m_inboxCache.end() && !cit->second.fDecryptFailed)
                continue; // valid cache hit — skip value deserialisation and decrypt

            smsg::SecMsgStored smsgStored;
            if (!db.ReadSmesg(chKey, smsgStored))
                continue;
            if (smsgStored.vchMessage.size() < smsg::SMSG_HDR_LEN)
                continue;

            ToDecrypt e;
            e.keyStr = ks;
            memcpy(e.chKey.data(), chKey, 30);
            e.stored = std::move(smsgStored);
            toDecrypt.push_back(std::move(e));
        }
        delete it;
    } // cs_smsgDB released before decrypt

    // Phase 2: evict stale cache entries.
    for (auto it = m_inboxCache.begin(); it != m_inboxCache.end(); ) {
        if (!currentKeys.count(it->first))
            it = m_inboxCache.erase(it);
        else
            ++it;
    }

    // Phase 3: decrypt and populate cache for new / failed entries.
    for (auto& e : toDecrypt) {
        uint8_t* pHeader = e.stored.vchMessage.data();
        const smsg::SecureMessage* psmsg = (smsg::SecureMessage*)pHeader;
        uint32_t nPayload = e.stored.vchMessage.size() - smsg::SMSG_HDR_LEN;

        DecryptedRow r;
        memcpy(r.chKey.data(), e.chKey.data(), 30);
        r.timeReceived   = e.stored.timeReceived;
        r.status         = e.stored.status;
        r.fUnread        = (e.stored.status & SMSG_MASK_UNREAD) != 0;
        r.fPaid          = psmsg->IsPaidVersion();
        r.nDaysRetention = r.fPaid ? psmsg->nonce[0] : 2;
        r.sMsgId         = QString::fromStdString(HexStr(Span<uint8_t>(&e.chKey[2], &e.chKey[2] + 28)));

        smsg::MessageData msg;
        int rv = smsgModule.Decrypt(false, e.stored.addrTo, pHeader, pHeader + smsg::SMSG_HDR_LEN, nPayload, msg);
        if (rv == 0) {
            r.sFrom          = QString::fromStdString(msg.sFromAddress);
            r.sTo            = QString::fromStdString(EncodeDestination(PKHash(e.stored.addrTo)));
            r.sText          = QString::fromStdString(std::string((char*)msg.vchMessage.data()));
            r.timeSent       = msg.timestamp;
            r.fDecryptFailed = false;
        } else {
            r.sText          = tr("[Decrypt failed: %1]").arg(QString::fromStdString(smsg::GetString(rv)));
            r.timeSent       = 0;
            r.fDecryptFailed = true;
        }
        m_inboxCache[e.keyStr] = std::move(r);
    }

    // Phase 4: rebuild table from cache.
    QTableWidget* table = ui->inboxTable;
    table->setSortingEnabled(false);
    table->setUpdatesEnabled(false);
    table->setRowCount(0);

    QString filterText = ui->inboxFilterLineEdit->text();
    int nMessages = 0;
    int nUnread = 0;

    for (const auto& ks : currentKeys) {
        auto it = m_inboxCache.find(ks);
        if (it == m_inboxCache.end())
            continue;
        const DecryptedRow& r = it->second;

        if (!filterText.isEmpty()) {
            if (!r.sFrom.contains(filterText, Qt::CaseInsensitive) &&
                !r.sTo.contains(filterText, Qt::CaseInsensitive) &&
                !r.sText.contains(filterText, Qt::CaseInsensitive))
                continue;
        }

        int row = table->rowCount();
        table->insertRow(row);

        QTableWidgetItem* readItem = new QTableWidgetItem(r.fUnread ? tr("*") : QString());
        readItem->setTextAlignment(Qt::AlignCenter);
        readItem->setData(Qt::UserRole + 1, QByteArray(reinterpret_cast<const char*>(r.chKey.data()), 30));
        table->setItem(row, INBOX_COL_READ, readItem);

        QTableWidgetItem* recvItem = new QTableWidgetItem(
            QDateTime::fromSecsSinceEpoch(r.timeReceived).toString("yyyy-MM-dd hh:mm"));
        recvItem->setData(Qt::UserRole, (qlonglong)r.timeReceived);
        table->setItem(row, INBOX_COL_DATE_RECV, recvItem);

        QTableWidgetItem* sentItem = new QTableWidgetItem(
            r.timeSent > 0 ? QDateTime::fromSecsSinceEpoch(r.timeSent).toString("yyyy-MM-dd hh:mm") : QString());
        sentItem->setData(Qt::UserRole, (qlonglong)r.timeSent);
        table->setItem(row, INBOX_COL_DATE_SENT, sentItem);

        table->setItem(row, INBOX_COL_FROM,      new QTableWidgetItem(r.sFrom));
        table->setItem(row, INBOX_COL_TO,         new QTableWidgetItem(r.sTo));
        table->setItem(row, INBOX_COL_PAID,       new QTableWidgetItem(r.fPaid ? tr("Yes") : tr("No")));

        QTableWidgetItem* retItem = new QTableWidgetItem(QString::number(r.nDaysRetention));
        retItem->setTextAlignment(Qt::AlignCenter);
        table->setItem(row, INBOX_COL_RETENTION, retItem);

        QString preview = r.sText.left(80).replace('\n', ' ');
        QTableWidgetItem* textItem = new QTableWidgetItem(preview);
        textItem->setData(Qt::UserRole, r.sText);
        table->setItem(row, INBOX_COL_TEXT,  textItem);
        table->setItem(row, INBOX_COL_MSGID, new QTableWidgetItem(r.sMsgId));

        if (r.fUnread) {
            QFont font = table->font();
            font.setBold(true);
            for (int col = 0; col < INBOX_COL_COUNT; ++col) {
                QTableWidgetItem* item = table->item(row, col);
                if (item) item->setFont(font);
            }
            nUnread++;
        }
        nMessages++;
    }

    table->setUpdatesEnabled(true);
    table->setSortingEnabled(true);
    ui->inboxCountLabel->setText(QString::number(nMessages) + tr(" messages"));
    ui->tabWidget->setTabText(0, nUnread > 0 ? tr("Inbox (%1)").arg(nUnread) : tr("Inbox"));
}

void MessagingPage::updateOutboxList()
{
    if (!smsg::fSecMsgEnabled)
        return;

    // Phase 1: key-only scan; read full stored only for cache misses / prior failures.
    struct ToDecrypt {
        std::string keyStr;
        std::array<uint8_t, 30> chKey;
        smsg::SecMsgStored stored;
    };
    std::vector<ToDecrypt> toDecrypt;
    std::unordered_set<std::string> currentKeys;

    {
        LOCK(smsg::cs_smsgDB);
        smsg::SecMsgDB db;
        if (!db.Open("cr+"))
            return;

        uint8_t chKey[30];
        std::string sPrefix("sm");
        leveldb::Iterator* it = db.pdb->NewIterator(leveldb::ReadOptions());
        while (db.NextSmesgKey(it, sPrefix, chKey)) {
            std::string ks(reinterpret_cast<const char*>(chKey), 30);

            auto cit = m_outboxCache.find(ks);
            if (cit != m_outboxCache.end() && !cit->second.fDecryptFailed) {
                // trollbox-destined copies are never shown in the outbox tab;
                // they were never inserted into m_outboxCache, so a hit here
                // is always a legitimate outbox entry.
                currentKeys.insert(ks);
                continue;
            }

            smsg::SecMsgStored smsgStored;
            if (!db.ReadSmesg(chKey, smsgStored))
                continue;
            if (smsgStored.vchMessage.size() < smsg::SMSG_HDR_LEN)
                continue;
            if (smsgStored.addrTo == smsgModule.trollboxAddress)
                continue;

            currentKeys.insert(ks);
            ToDecrypt e;
            e.keyStr = ks;
            memcpy(e.chKey.data(), chKey, 30);
            e.stored = std::move(smsgStored);
            toDecrypt.push_back(std::move(e));
        }
        delete it;
    } // cs_smsgDB released before decrypt

    // Phase 2: evict stale cache entries.
    for (auto it = m_outboxCache.begin(); it != m_outboxCache.end(); ) {
        if (!currentKeys.count(it->first))
            it = m_outboxCache.erase(it);
        else
            ++it;
    }

    // Phase 3: decrypt and populate cache for new / failed entries.
    for (auto& e : toDecrypt) {
        uint8_t* pHeader = e.stored.vchMessage.data();
        const smsg::SecureMessage* psmsg = (smsg::SecureMessage*)pHeader;
        uint32_t nPayload = e.stored.vchMessage.size() - smsg::SMSG_HDR_LEN;

        DecryptedRow r;
        memcpy(r.chKey.data(), e.chKey.data(), 30);
        r.timeReceived   = e.stored.timeReceived;
        r.status         = e.stored.status;
        r.fPaid          = psmsg->IsPaidVersion();
        r.nDaysRetention = r.fPaid ? psmsg->nonce[0] : 2;
        r.sMsgId         = QString::fromStdString(HexStr(Span<uint8_t>(&e.chKey[2], &e.chKey[2] + 28)));

        smsg::MessageData msg;
        int rv = smsgModule.Decrypt(false, e.stored.addrOutbox, pHeader, pHeader + smsg::SMSG_HDR_LEN, nPayload, msg);
        if (rv == 0) {
            r.sFrom          = QString::fromStdString(msg.sFromAddress);
            r.sTo            = QString::fromStdString(EncodeDestination(PKHash(e.stored.addrTo)));
            r.sText          = QString::fromStdString(std::string((char*)msg.vchMessage.data()));
            r.timeSent       = msg.timestamp;
            r.fDecryptFailed = false;
        } else {
            r.sText          = tr("[Decrypt failed: %1]").arg(QString::fromStdString(smsg::GetString(rv)));
            r.timeSent       = 0;
            r.fDecryptFailed = true;
        }
        m_outboxCache[e.keyStr] = std::move(r);
    }

    // Phase 4: rebuild table from cache.
    QTableWidget* table = ui->outboxTable;
    table->setSortingEnabled(false);
    table->setUpdatesEnabled(false);
    table->setRowCount(0);

    QString filterText = ui->outboxFilterLineEdit->text();
    int nMessages = 0;

    for (const auto& ks : currentKeys) {
        auto it = m_outboxCache.find(ks);
        if (it == m_outboxCache.end())
            continue;
        const DecryptedRow& r = it->second;

        if (!filterText.isEmpty()) {
            if (!r.sFrom.contains(filterText, Qt::CaseInsensitive) &&
                !r.sTo.contains(filterText, Qt::CaseInsensitive) &&
                !r.sText.contains(filterText, Qt::CaseInsensitive))
                continue;
        }

        int row = table->rowCount();
        table->insertRow(row);

        QTableWidgetItem* sentItem = new QTableWidgetItem(
            r.timeSent > 0 ? QDateTime::fromSecsSinceEpoch(r.timeSent).toString("yyyy-MM-dd hh:mm") : QString());
        sentItem->setData(Qt::UserRole,     (qlonglong)r.timeSent);
        sentItem->setData(Qt::UserRole + 1, QByteArray(reinterpret_cast<const char*>(r.chKey.data()), 30));
        table->setItem(row, OUTBOX_COL_DATE_SENT, sentItem);

        table->setItem(row, OUTBOX_COL_FROM, new QTableWidgetItem(r.sFrom));
        table->setItem(row, OUTBOX_COL_TO,   new QTableWidgetItem(r.sTo));
        table->setItem(row, OUTBOX_COL_PAID, new QTableWidgetItem(r.fPaid ? tr("Yes") : tr("No")));

        QTableWidgetItem* retItem = new QTableWidgetItem(QString::number(r.nDaysRetention));
        retItem->setTextAlignment(Qt::AlignCenter);
        table->setItem(row, OUTBOX_COL_RETENTION, retItem);

        QString preview = r.sText.left(80).replace('\n', ' ');
        QTableWidgetItem* textItem = new QTableWidgetItem(preview);
        textItem->setData(Qt::UserRole, r.sText);
        table->setItem(row, OUTBOX_COL_TEXT,  textItem);
        table->setItem(row, OUTBOX_COL_MSGID, new QTableWidgetItem(r.sMsgId));

        nMessages++;
    }

    table->setUpdatesEnabled(true);
    table->setSortingEnabled(true);
    ui->outboxCountLabel->setText(QString::number(nMessages) + tr(" messages"));
}

void MessagingPage::updateKeysList()
{
    if (!smsg::fSecMsgEnabled)
        return;

    QTableWidget* table = ui->keysTable;
    table->setRowCount(0);

    QString filterText = ui->keysFilterLineEdit->text();

    struct OwnAddrSnap { CKeyID address; bool fRecv; bool fAnon; };
    struct KeySnap {
        CKeyID address;
        std::string sLabel;
        bool fContact;
        bool fRecv;
        bool fAnon;
        bool fHasKey;
        CKey key;
    };
    std::vector<OwnAddrSnap> ownSnap;
    std::vector<KeySnap> keySnap;
    bool havePwallet = false;

    {
        LOCK(smsgModule.cs_smsg);
        havePwallet = (smsgModule.pwallet != nullptr);
#ifdef ENABLE_WALLET
        if (smsgModule.pwallet) {
            for (const auto& it : smsgModule.addresses)
                ownSnap.push_back({it.address, it.fReceiveEnabled, it.fReceiveAnon});
        }
#endif
        for (const auto& p : smsgModule.keyStore.mapKeys) {
            if (p.first == smsgModule.trollboxAddress)
                continue;
            const auto& key = p.second;
            KeySnap ks;
            ks.address  = p.first;
            ks.sLabel   = key.sLabel;
            ks.fContact = (key.nFlags & smsg::SMK_CONTACT_ONLY) != 0;
            ks.fRecv    = (key.nFlags & smsg::SMK_RECEIVE_ON) != 0;
            ks.fAnon    = (key.nFlags & smsg::SMK_RECEIVE_ANON) != 0;
            ks.fHasKey  = key.key.IsValid();
            ks.key      = key.key;
            keySnap.push_back(std::move(ks));
        }
    } // cs_smsg released

#ifdef ENABLE_WALLET
    if (havePwallet && smsgModule.pwallet) {
        auto* spk_man = smsgModule.pwallet->GetLegacyScriptPubKeyMan();
        for (const auto& e : ownSnap) {
            QString sAddr = QString::fromStdString(EncodeDestination(PKHash(e.address)));
            PKHash pkh = PKHash(e.address);
            QString sLabel = QString::fromStdString(smsgModule.LookupLabel(pkh));

            if (!filterText.isEmpty()) {
                if (!sAddr.contains(filterText, Qt::CaseInsensitive) &&
                    !sLabel.contains(filterText, Qt::CaseInsensitive))
                    continue;
            }

            QString sPubKey;
            if (spk_man) {
                LOCK(smsgModule.pwallet->cs_wallet);
                CPubKey pubKey;
                if (spk_man->GetPubKey(CKeyID(e.address), pubKey) && pubKey.IsValid())
                    sPubKey = QString::fromStdString(HexStr(pubKey));
            }

            int row = table->rowCount();
            table->insertRow(row);
            QTableWidgetItem* addrItem = new QTableWidgetItem(sAddr);
            addrItem->setData(Qt::UserRole, sPubKey);
            table->setItem(row, KEYS_COL_ADDRESS, addrItem);
            table->setItem(row, KEYS_COL_LABEL, new QTableWidgetItem(sLabel));
            table->setItem(row, KEYS_COL_RECEIVE, new QTableWidgetItem(e.fRecv ? tr("On") : tr("Off")));
            table->setItem(row, KEYS_COL_ANON, new QTableWidgetItem(e.fAnon ? tr("On") : tr("Off")));
            table->setItem(row, KEYS_COL_SOURCE, new QTableWidgetItem(tr("My Key")));
            QColor ownKeyColor(210, 240, 210);
            for (int col = 0; col < KEYS_COL_COUNT; ++col) {
                if (table->item(row, col))
                    table->item(row, col)->setBackground(QBrush(ownKeyColor));
            }
        }
    }
#endif

    {
        LOCK(smsg::cs_smsgDB);
        smsg::SecMsgDB db;
        bool dbOpen = db.Open("cr+");

        for (const auto& ks : keySnap) {
            QString sAddr = QString::fromStdString(EncodeDestination(PKHash(ks.address)));
            QString sLabel = QString::fromStdString(ks.sLabel);

            if (!filterText.isEmpty()) {
                if (!sAddr.contains(filterText, Qt::CaseInsensitive) &&
                    !sLabel.contains(filterText, Qt::CaseInsensitive))
                    continue;
            }

            QString sPubKey;
            if (ks.fContact && dbOpen) {
                CPubKey pubKey;
                if (db.ReadPK(ks.address, pubKey) && pubKey.IsValid())
                    sPubKey = QString::fromStdString(HexStr(pubKey));
            } else if (ks.fHasKey) {
                CPubKey pubKey = ks.key.GetPubKey();
                if (pubKey.IsValid())
                    sPubKey = QString::fromStdString(HexStr(pubKey));
            }

            int row = table->rowCount();
            table->insertRow(row);
            QTableWidgetItem* addrItem = new QTableWidgetItem(sAddr);
            addrItem->setData(Qt::UserRole, sPubKey);
            table->setItem(row, KEYS_COL_ADDRESS, addrItem);
            table->setItem(row, KEYS_COL_LABEL, new QTableWidgetItem(sLabel));
            if (ks.fContact) {
                table->setItem(row, KEYS_COL_RECEIVE, new QTableWidgetItem(tr("N/A")));
                table->setItem(row, KEYS_COL_ANON, new QTableWidgetItem(tr("N/A")));
                table->setItem(row, KEYS_COL_SOURCE, new QTableWidgetItem(tr("Contact")));
            } else {
                table->setItem(row, KEYS_COL_RECEIVE, new QTableWidgetItem(ks.fRecv ? tr("On") : tr("Off")));
                table->setItem(row, KEYS_COL_ANON, new QTableWidgetItem(ks.fAnon ? tr("On") : tr("Off")));
                table->setItem(row, KEYS_COL_SOURCE, new QTableWidgetItem(tr("My Key")));
            }
        }
    } // cs_smsgDB
}

void MessagingPage::updateFromAddresses()
{
    if (!smsg::fSecMsgEnabled)
        return;

    m_lastFeeEstimate = 0;
    QString current = ui->fromAddressCombo->currentText();
    ui->fromAddressCombo->clear();

    QSet<QString> seen;
    {
        LOCK(smsgModule.cs_smsg);

#ifdef ENABLE_WALLET
        if (smsgModule.pwallet) {
            auto* spk_man = smsgModule.pwallet->GetLegacyScriptPubKeyMan();
            for (auto it = smsgModule.addresses.begin(); it != smsgModule.addresses.end(); ++it) {
                if (!it->fReceiveEnabled)
                    continue;
                if (!spk_man || !spk_man->HaveKey(it->address))
                    continue;
                QString qAddr = QString::fromStdString(EncodeDestination(PKHash(it->address)));
                seen.insert(qAddr);
                ui->fromAddressCombo->addItem(qAddr);
            }
        }
#endif

        for (auto& p : smsgModule.keyStore.mapKeys) {
            if (p.first == smsgModule.trollboxAddress)
                continue;
            auto& key = p.second;
            if (key.nFlags & smsg::SMK_CONTACT_ONLY)
                continue;
            if (!(key.nFlags & smsg::SMK_RECEIVE_ON))
                continue;
            if (!key.key.IsValid())
                continue;
            QString qAddr = QString::fromStdString(EncodeDestination(PKHash(p.first)));
            if (!seen.contains(qAddr)) {
                seen.insert(qAddr);
                ui->fromAddressCombo->addItem(qAddr);
            }
        }
    }

    int idx = ui->fromAddressCombo->findText(current);
    if (idx >= 0)
        ui->fromAddressCombo->setCurrentIndex(idx);
}

void MessagingPage::onPaidToggled(bool checked)
{
    ui->retentionSpinBox->setEnabled(checked);
    updateFeeEstimate();
    onMessageTextChanged(); // update char counter for new limit
}

void MessagingPage::onRetentionChanged(int /*value*/)
{
    updateFeeEstimate();
}

void MessagingPage::onMessageTextChanged()
{
    m_lastFeeEstimate = 0;
    bool fPaid = ui->paidCheckBox->isChecked();
    unsigned int maxLen = fPaid ? smsg::SMSG_MAX_MSG_BYTES_PAID : smsg::SMSG_MAX_MSG_BYTES;
    int len = ui->messageEdit->toPlainText().toUtf8().size();
    ui->charCountLabel->setText(QString::number(len) + " / " + QString::number(maxLen));

    if ((unsigned int)len > maxLen) {
        ui->charCountLabel->setStyleSheet("QLabel { color: red; }");
    } else {
        ui->charCountLabel->setStyleSheet("");
    }
}

void MessagingPage::updateFeeEstimate()
{
    if (!ui->paidCheckBox->isChecked()) {
        ui->feeEstimateLabel->setText("");
        return;
    }

    if (!smsg::fSecMsgEnabled) {
        ui->feeEstimateLabel->setText(tr("SMSG disabled"));
        return;
    }

    QString addrFrom = ui->fromAddressCombo->currentText();
    QString addrTo = ui->toAddressEdit->text();

    if (addrFrom.isEmpty() || addrTo.isEmpty()) {
        ui->feeEstimateLabel->setText(tr("Enter addresses to estimate fee"));
        return;
    }

    // Arithmetic estimate — avoids Encrypt + FundTransaction per UI event.
    // Formula mirrors FundMsg: nMsgFee = ((nMsgFeePerKPerDay * nBytes) / 1000) * nDays
    // Plus approx funding-tx network fee (~200 bytes at nFundingTxnFeePerK).
    size_t nTextBytes = (size_t)ui->messageEdit->toPlainText().toUtf8().size();
    if (nTextBytes == 0) nTextBytes = 1;
    // SMSG_HDR_LEN + encrypted payload upper bound (PL_HDR + text + AES block + paid tail)
    size_t nApproxBytes = smsg::SMSG_HDR_LEN + smsg::SMSG_PL_HDR_LEN + nTextBytes + 48;
    int nRetention = ui->retentionSpinBox->value();
    CAmount nMsgFee = ((smsg::nMsgFeePerKPerDay * (CAmount)nApproxBytes) / 1000) * nRetention;
    CAmount nTxFee  = ((CAmount)smsg::nFundingTxnFeePerK * 200) / 1000;
    m_lastFeeEstimate = nMsgFee + nTxFee;
    ui->feeEstimateLabel->setText(tr("Estimated fee: ~%1 OMEGA").arg(
        QString::number((double)m_lastFeeEstimate / 100000000.0, 'f', 8)));
}

void MessagingPage::onSendClicked()
{
    if (!smsg::fSecMsgEnabled) {
        ui->sendStatusLabel->setText(tr("Secure messaging is not enabled."));
        return;
    }

    QString addrFrom = ui->fromAddressCombo->currentText();
    QString addrTo = ui->toAddressEdit->text();
    QString message = ui->messageEdit->toPlainText();

    if (addrFrom.isEmpty()) {
        ui->sendStatusLabel->setText(tr("Select a From address."));
        return;
    }
    if (addrTo.isEmpty()) {
        ui->sendStatusLabel->setText(tr("Enter a To address."));
        return;
    }
    if (message.isEmpty()) {
        ui->sendStatusLabel->setText(tr("Enter a message."));
        return;
    }

    bool fPaid = ui->paidCheckBox->isChecked();
    unsigned int maxLen = fPaid ? smsg::SMSG_MAX_MSG_BYTES_PAID : smsg::SMSG_MAX_MSG_BYTES;
    if ((unsigned int)message.toUtf8().size() > maxLen) {
        ui->sendStatusLabel->setText(tr("Message too long."));
        return;
    }

    CTxDestination destFrom = DecodeDestination(addrFrom.toStdString());
    CTxDestination destTo = DecodeDestination(addrTo.toStdString());

    if (!IsValidDestination(destFrom)) {
        ui->sendStatusLabel->setText(tr("Invalid From address."));
        return;
    }
    if (!IsValidDestination(destTo)) {
        ui->sendStatusLabel->setText(tr("Invalid To address."));
        return;
    }

    const PKHash *pkhFrom = std::get_if<PKHash>(&destFrom);
    const PKHash *pkhTo = std::get_if<PKHash>(&destTo);
    if (!pkhFrom || !pkhTo) {
        ui->sendStatusLabel->setText(tr("SMSG requires P2PKH addresses."));
        return;
    }
    CKeyID kiFrom = ToKeyID(*pkhFrom);
    CKeyID kiTo = ToKeyID(*pkhTo);

    std::string sMessage = message.toStdString();
    std::string sError;
    smsg::SecureMessage smsgOut;
    CAmount nFee = 0;
    int nRetention = ui->retentionSpinBox->value();

    if (fPaid) {
        if (m_lastFeeEstimate > 0) {
            nFee = m_lastFeeEstimate;
        } else {
            int rv_fee = smsgModule.Send(kiFrom, kiTo, sMessage, smsgOut, sError, true, nRetention, true, &nFee);
            if (rv_fee != 0) {
                ui->sendStatusLabel->setText(tr("Send failed: %1").arg(QString::fromStdString(sError)));
                return;
            }
        }

        QMessageBox::StandardButton reply = QMessageBox::question(this, tr("Confirm Paid Message"),
            tr("Send paid message?\nFee: %1 OMEGA\nRetention: %2 days")
                .arg(QString::number((double)nFee / 100000000.0, 'f', 8))
                .arg(nRetention),
            QMessageBox::Yes | QMessageBox::No);

        if (reply != QMessageBox::Yes)
            return;
    }

    int rv = smsgModule.Send(kiFrom, kiTo, sMessage, smsgOut, sError, fPaid, nRetention);

    if (rv != 0) {
        ui->sendStatusLabel->setText(tr("Send failed: %1").arg(QString::fromStdString(sError)));
        return;
    }

    ui->sendStatusLabel->setText(tr("Message sent successfully."));
    ui->messageEdit->clear();
    ui->toAddressEdit->clear();
    ui->paidCheckBox->setChecked(false);
}

// ----- Context menu handlers -----

void MessagingPage::showInboxContextMenu(const QPoint& point)
{
    QTableWidgetItem* item = ui->inboxTable->itemAt(point);
    if (item)
        inboxContextMenu->exec(QCursor::pos());
}

void MessagingPage::showOutboxContextMenu(const QPoint& point)
{
    QTableWidgetItem* item = ui->outboxTable->itemAt(point);
    if (item)
        outboxContextMenu->exec(QCursor::pos());
}

void MessagingPage::showKeysContextMenu(const QPoint& point)
{
    QTableWidgetItem* item = ui->keysTable->itemAt(point);
    if (item) {
        int row = ui->keysTable->row(item);
        QTableWidgetItem* sourceItem = ui->keysTable->item(row, KEYS_COL_SOURCE);
        bool isContact = sourceItem && sourceItem->text() == tr("Contact");
        sendMessageAction->setVisible(isContact);
        deleteContactAction->setVisible(isContact);
        keysContextMenu->exec(QCursor::pos());
    }
}

void MessagingPage::showInboxMessage()
{
    int row = ui->inboxTable->currentRow();
    if (row < 0) return;
    QTableWidgetItem* textItem = ui->inboxTable->item(row, INBOX_COL_TEXT);
    if (!textItem) return;

    QString fullText = textItem->data(Qt::UserRole).toString();
    if (fullText.isEmpty())
        fullText = textItem->text();

    QString from;
    QTableWidgetItem* fromItem = ui->inboxTable->item(row, INBOX_COL_FROM);
    if (fromItem) from = fromItem->text();

    QMessageBox msgBox(this);
    msgBox.setWindowTitle(tr("Message from %1").arg(from));
    msgBox.setText(fullText);
    msgBox.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    msgBox.exec();
}

void MessagingPage::copyFromAddress()
{
    int row = ui->inboxTable->currentRow();
    if (row < 0) return;
    QTableWidgetItem* item = ui->inboxTable->item(row, INBOX_COL_FROM);
    if (item)
        QApplication::clipboard()->setText(item->text());
}

void MessagingPage::copyToAddress()
{
    int row = ui->inboxTable->currentRow();
    if (row < 0) return;
    QTableWidgetItem* item = ui->inboxTable->item(row, INBOX_COL_TO);
    if (item)
        QApplication::clipboard()->setText(item->text());
}

void MessagingPage::copyMessageId()
{
    int row = ui->inboxTable->currentRow();
    if (row < 0) return;
    QTableWidgetItem* item = ui->inboxTable->item(row, INBOX_COL_MSGID);
    if (item)
        QApplication::clipboard()->setText(item->text());
}

void MessagingPage::markRead()
{
    int row = ui->inboxTable->currentRow();
    if (row < 0) return;
    QTableWidgetItem* readItem = ui->inboxTable->item(row, INBOX_COL_READ);
    if (!readItem) return;
    QByteArray dbKey = readItem->data(Qt::UserRole + 1).toByteArray();
    if (dbKey.size() != 30) return;

    {
        LOCK(smsg::cs_smsgDB);
        smsg::SecMsgDB dbInbox;
        if (!dbInbox.Open("cr+"))
            return;

        uint8_t chKey[30];
        memcpy(chKey, dbKey.constData(), 30);
        smsg::SecMsgStored smsgStored;
        if (dbInbox.ReadSmesg(chKey, smsgStored)) {
            smsgStored.status &= ~SMSG_MASK_UNREAD;
            dbInbox.WriteSmesg(chKey, smsgStored);
        }
    }
    {
        std::string ks(dbKey.constData(), 30);
        auto it = m_inboxCache.find(ks);
        if (it != m_inboxCache.end()) {
            it->second.status  &= ~SMSG_MASK_UNREAD;
            it->second.fUnread  = false;
        }
    }
    updateInboxList();
}

void MessagingPage::markUnread()
{
    int row = ui->inboxTable->currentRow();
    if (row < 0) return;
    QTableWidgetItem* readItem = ui->inboxTable->item(row, INBOX_COL_READ);
    if (!readItem) return;
    QByteArray dbKey = readItem->data(Qt::UserRole + 1).toByteArray();
    if (dbKey.size() != 30) return;

    {
        LOCK(smsg::cs_smsgDB);
        smsg::SecMsgDB dbInbox;
        if (!dbInbox.Open("cr+"))
            return;

        uint8_t chKey[30];
        memcpy(chKey, dbKey.constData(), 30);
        smsg::SecMsgStored smsgStored;
        if (dbInbox.ReadSmesg(chKey, smsgStored)) {
            smsgStored.status |= SMSG_MASK_UNREAD;
            dbInbox.WriteSmesg(chKey, smsgStored);
        }
    }
    {
        std::string ks(dbKey.constData(), 30);
        auto it = m_inboxCache.find(ks);
        if (it != m_inboxCache.end()) {
            it->second.status |= SMSG_MASK_UNREAD;
            it->second.fUnread = true;
        }
    }
    updateInboxList();
}

void MessagingPage::deleteSelectedInbox()
{
    int row = ui->inboxTable->currentRow();
    if (row < 0) return;
    QTableWidgetItem* readItem = ui->inboxTable->item(row, INBOX_COL_READ);
    if (!readItem) return;
    QByteArray dbKey = readItem->data(Qt::UserRole + 1).toByteArray();
    if (dbKey.size() != 30) return;

    {
        LOCK(smsg::cs_smsgDB);
        smsg::SecMsgDB dbInbox;
        if (!dbInbox.Open("cr+"))
            return;

        uint8_t chKey[30];
        memcpy(chKey, dbKey.constData(), 30);
        dbInbox.EraseSmesg(chKey);
    }
    m_inboxCache.erase(std::string(dbKey.constData(), 30));
    updateInboxList();
}

void MessagingPage::purgeSelectedInbox()
{
    int row = ui->inboxTable->currentRow();
    if (row < 0) return;
    QTableWidgetItem* msgIdItem = ui->inboxTable->item(row, INBOX_COL_MSGID);
    if (!msgIdItem) return;

    std::string sMsgId = msgIdItem->text().toStdString();
    if (!IsHex(sMsgId) || sMsgId.size() != 56)
        return;

    QMessageBox::StandardButton reply = QMessageBox::question(this, tr("Purge Message"),
        tr("Purge this message from the network? This cannot be undone."),
        QMessageBox::Yes | QMessageBox::No);

    if (reply != QMessageBox::Yes)
        return;

    std::vector<uint8_t> vMsgId = ParseHex(sMsgId.c_str());
    std::string sError;
    if (smsg::SMSG_NO_ERROR != smsgModule.Purge(vMsgId, sError)) {
        QMessageBox::warning(this, tr("Purge Failed"), QString::fromStdString(sError));
        return;
    }

    updateInboxList();
}

void MessagingPage::showOutboxMessage()
{
    int row = ui->outboxTable->currentRow();
    if (row < 0) return;
    QTableWidgetItem* textItem = ui->outboxTable->item(row, OUTBOX_COL_TEXT);
    if (!textItem) return;

    QString fullText = textItem->data(Qt::UserRole).toString();
    if (fullText.isEmpty())
        fullText = textItem->text();

    QString to;
    QTableWidgetItem* toItem = ui->outboxTable->item(row, OUTBOX_COL_TO);
    if (toItem) to = toItem->text();

    QMessageBox msgBox(this);
    msgBox.setWindowTitle(tr("Message to %1").arg(to));
    msgBox.setText(fullText);
    msgBox.setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    msgBox.exec();
}

void MessagingPage::copyOutboxFromAddress()
{
    int row = ui->outboxTable->currentRow();
    if (row < 0) return;
    QTableWidgetItem* item = ui->outboxTable->item(row, OUTBOX_COL_FROM);
    if (item)
        QApplication::clipboard()->setText(item->text());
}

void MessagingPage::copyOutboxToAddress()
{
    int row = ui->outboxTable->currentRow();
    if (row < 0) return;
    QTableWidgetItem* item = ui->outboxTable->item(row, OUTBOX_COL_TO);
    if (item)
        QApplication::clipboard()->setText(item->text());
}

void MessagingPage::copyOutboxMessageId()
{
    int row = ui->outboxTable->currentRow();
    if (row < 0) return;
    QTableWidgetItem* item = ui->outboxTable->item(row, OUTBOX_COL_MSGID);
    if (item)
        QApplication::clipboard()->setText(item->text());
}

void MessagingPage::deleteSelectedOutbox()
{
    int row = ui->outboxTable->currentRow();
    if (row < 0) return;
    QTableWidgetItem* sentItem = ui->outboxTable->item(row, OUTBOX_COL_DATE_SENT);
    if (!sentItem) return;
    QByteArray dbKey = sentItem->data(Qt::UserRole + 1).toByteArray();
    if (dbKey.size() != 30) return;

    {
        LOCK(smsg::cs_smsgDB);
        smsg::SecMsgDB dbOutbox;
        if (!dbOutbox.Open("cr+"))
            return;

        uint8_t chKey[30];
        memcpy(chKey, dbKey.constData(), 30);
        dbOutbox.EraseSmesg(chKey);
    }
    m_outboxCache.erase(std::string(dbKey.constData(), 30));
    updateOutboxList();
}

// ----- Keys context menu actions -----

void MessagingPage::toggleReceive()
{
    int row = ui->keysTable->currentRow();
    if (row < 0) return;

    QTableWidgetItem* addrItem = ui->keysTable->item(row, KEYS_COL_ADDRESS);
    QTableWidgetItem* recvItem = ui->keysTable->item(row, KEYS_COL_RECEIVE);
    if (!addrItem || !recvItem) return;

    std::string sAddr = addrItem->text().toStdString();
    bool fCurrentlyOn = (recvItem->text() == tr("On"));
    bool fNewValue = !fCurrentlyOn;

    CTxDestination dest = DecodeDestination(sAddr);
    const PKHash *pkh = std::get_if<PKHash>(&dest);
    if (!pkh) return;
    CKeyID keyID = ToKeyID(*pkh);

    if (!smsgModule.SetWalletAddressOption(keyID, "receive", fNewValue) &&
        !smsgModule.SetSmsgAddressOption(keyID, "receive", fNewValue)) {
        QMessageBox::warning(this, tr("Error"), tr("Failed to update receive option."));
        return;
    }

    updateKeysList();
}

void MessagingPage::toggleAnon()
{
    int row = ui->keysTable->currentRow();
    if (row < 0) return;

    QTableWidgetItem* addrItem = ui->keysTable->item(row, KEYS_COL_ADDRESS);
    QTableWidgetItem* anonItem = ui->keysTable->item(row, KEYS_COL_ANON);
    if (!addrItem || !anonItem) return;

    std::string sAddr = addrItem->text().toStdString();
    bool fCurrentlyOn = (anonItem->text() == tr("On"));
    bool fNewValue = !fCurrentlyOn;

    CTxDestination dest = DecodeDestination(sAddr);
    const PKHash *pkh = std::get_if<PKHash>(&dest);
    if (!pkh) return;
    CKeyID keyID = ToKeyID(*pkh);

    if (!smsgModule.SetWalletAddressOption(keyID, "anon", fNewValue) &&
        !smsgModule.SetSmsgAddressOption(keyID, "anon", fNewValue)) {
        QMessageBox::warning(this, tr("Error"), tr("Failed to update anon option."));
        return;
    }

    updateKeysList();
}

void MessagingPage::copyKeyAddress()
{
    int row = ui->keysTable->currentRow();
    if (row < 0) return;
    QTableWidgetItem* item = ui->keysTable->item(row, KEYS_COL_ADDRESS);
    if (item)
        QApplication::clipboard()->setText(item->text());
}

void MessagingPage::copyKeyPublicKey()
{
    int row = ui->keysTable->currentRow();
    if (row < 0) return;
    QTableWidgetItem* item = ui->keysTable->item(row, KEYS_COL_ADDRESS);
    if (!item) return;
    QString pubkey = item->data(Qt::UserRole).toString();
    if (pubkey.isEmpty()) {
        QMessageBox::warning(this, tr("No Public Key"),
            tr("Public key is not available for this entry."));
        return;
    }
    QApplication::clipboard()->setText(pubkey);
}

void MessagingPage::sendMessageToSelected()
{
    int row = ui->keysTable->currentRow();
    if (row < 0) return;
    QTableWidgetItem* addrItem = ui->keysTable->item(row, KEYS_COL_ADDRESS);
    if (!addrItem) return;
    ui->toAddressEdit->setText(addrItem->text());
    ui->tabWidget->setCurrentIndex(2); // Compose tab
}

void MessagingPage::deleteContact()
{
    int row = ui->keysTable->currentRow();
    if (row < 0) return;

    QTableWidgetItem* sourceItem = ui->keysTable->item(row, KEYS_COL_SOURCE);
    if (!sourceItem || sourceItem->text() != tr("Contact"))
        return;

    QTableWidgetItem* addrItem = ui->keysTable->item(row, KEYS_COL_ADDRESS);
    if (!addrItem) return;

    QString sAddr = addrItem->text();

    QMessageBox::StandardButton reply = QMessageBox::question(this, tr("Delete Contact"),
        tr("Delete contact %1?").arg(sAddr),
        QMessageBox::Yes | QMessageBox::No);
    if (reply != QMessageBox::Yes)
        return;

    std::string address = sAddr.toStdString();
    CTxDestination dest = DecodeDestination(address);
    const PKHash *pkHash = std::get_if<PKHash>(&dest);
    if (!pkHash) return;

    CKeyID idk(*pkHash);

    {
        LOCK(smsg::cs_smsgDB);
        smsg::SecMsgDB db;
        if (db.Open("cr+")) {
            db.EraseKey(idk);
        }
    }

    {
        LOCK(smsgModule.cs_smsg);
        smsgModule.keyStore.mapKeys.erase(idk);
    }

    updateKeysList();
}

void MessagingPage::onScanChainClicked()
{
    if (!smsg::fSecMsgEnabled) {
        QMessageBox::warning(this, tr("Messaging"), tr("Secure messaging is not enabled."));
        return;
    }

    if (m_fScanning) {
        // Abort a running scan.
        smsgModule.m_fScanAbort.store(true, std::memory_order_release);
        ui->scanChainButton->setEnabled(false);
        ui->scanChainButton->setText(tr("Stopping..."));
        return;
    }

    // Join any previous scan thread before starting a new one.
    if (m_scanThread.joinable())
        m_scanThread.join();

    m_fScanning = true;
    ui->scanChainButton->setText(tr("Stop Scan"));

    m_scanThread = std::thread([this]() {
        bool fResult = smsgModule.ScanBlockChain();
        m_fScanning = false;

        QMetaObject::invokeMethod(this, [this, fResult]() {
            ui->scanChainButton->setEnabled(true);
            ui->scanChainButton->setText(tr("Scan Blockchain"));

            if (smsgModule.m_fScanAbort.load(std::memory_order_acquire)) {
                smsgModule.m_fScanAbort.store(false, std::memory_order_release);
                QMessageBox::information(this, tr("Scan Stopped"), tr("Blockchain scan was stopped. Progress has been saved."));
            } else if (fResult) {
                QMessageBox::information(this, tr("Scan Complete"), tr("Blockchain scan completed successfully."));
            } else {
                QMessageBox::warning(this, tr("Scan Failed"), tr("Blockchain scan failed."));
            }

            updateKeysList();
        }, Qt::QueuedConnection);
    });
}

void MessagingPage::onAddAddressClicked()
{
    if (!smsg::fSecMsgEnabled) {
        QMessageBox::warning(this, tr("Messaging"), tr("Secure messaging is not enabled."));
        return;
    }

    bool ok;
    QString address = QInputDialog::getText(this, tr("Add Contact"),
        tr("Enter address:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || address.isEmpty())
        return;

    // Try to retrieve the public key from the scanned blockchain data.
    std::string sAddr = address.toStdString();
    std::string sPubKey;

    CTxDestination dest = DecodeDestination(sAddr);
    const PKHash *pkHash = std::get_if<PKHash>(&dest);
    if (pkHash) {
        CKeyID keyID(*pkHash);
        CPubKey cpk;
        if (smsgModule.GetStoredKey(keyID, cpk) == smsg::SMSG_NO_ERROR) {
            sPubKey = HexStr(cpk);
        }
    }

    if (sPubKey.empty()) {
        QString pubkey = QInputDialog::getText(this, tr("Add Contact"),
            tr("Enter public key (hex):"), QLineEdit::Normal, QString(), &ok);
        if (!ok || pubkey.isEmpty())
            return;
        sPubKey = pubkey.toStdString();
    }

    QString label = QInputDialog::getText(this, tr("Add Contact"),
        tr("Label:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || label.isEmpty())
        return;

    std::string sLabel = label.toStdString();

    int rv = smsgModule.AddContact(sAddr, sPubKey, sLabel);
    if (rv != 0) {
        QMessageBox::warning(this, tr("Error"),
            tr("Failed to add contact: %1").arg(QString::fromStdString(smsg::GetString(rv))));
        return;
    }

    QMessageBox::information(this, tr("Success"), tr("Contact added successfully."));
    updateKeysList();
}

void MessagingPage::replyToSelected()
{
    int row = ui->inboxTable->currentRow();
    if (row < 0) return;

    QTableWidgetItem* fromItem = ui->inboxTable->item(row, INBOX_COL_FROM);
    QTableWidgetItem* toItem   = ui->inboxTable->item(row, INBOX_COL_TO);
    if (!fromItem || !toItem) return;

    QString replyTo   = fromItem->text(); // original sender becomes our recipient
    QString replyFrom = toItem->text();   // address that received the message

    // Switch to Compose tab first so updateFromAddresses() is called
    ui->tabWidget->setCurrentIndex(2);

    ui->toAddressEdit->setText(replyTo);

    int idx = ui->fromAddressCombo->findText(replyFrom);
    if (idx >= 0)
        ui->fromAddressCombo->setCurrentIndex(idx);
}

void MessagingPage::onGenerateKeyClicked()
{
    if (!smsg::fSecMsgEnabled) {
        QMessageBox::warning(this, tr("Messaging"), tr("Secure messaging is not enabled."));
        return;
    }

    bool ok;
    QString label = QInputDialog::getText(this, tr("Generate Key"),
        tr("Label:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || label.isEmpty())
        return;

#ifdef ENABLE_WALLET
    // Generate key inside the wallet so the private key is wallet-managed and
    // survives backup/restore. Then register the address with SMSG.
    if (smsgModule.pwallet) {
        if (smsgModule.pwallet->IsLocked()) {
            QMessageBox::warning(this, tr("Wallet Locked"),
                tr("Please unlock the wallet before generating a messaging address.\n\n"
                   "Settings → Unlock Wallet"));
            return;
        }
        CTxDestination dest;
        std::string error;
        if (!smsgModule.pwallet->GetNewDestination(label.toStdString(), dest, error)) {
            QMessageBox::warning(this, tr("Error"),
                tr("Failed to generate wallet address: %1").arg(QString::fromStdString(error)));
            return;
        }

        std::string sAddr = EncodeDestination(dest);
        int rv = smsgModule.AddLocalAddress(sAddr);
        if (rv != smsg::SMSG_NO_ERROR) {
            QMessageBox::warning(this, tr("Error"),
                tr("Failed to register address with SMSG: %1").arg(QString::fromStdString(smsg::GetString(rv))));
            return;
        }

        QMessageBox::information(this, tr("Key Generated"),
            tr("New messaging address created and registered.\n\nAddress:\n%1\n\n"
               "The private key is stored in your wallet and protected by your wallet backup.")
            .arg(QString::fromStdString(sAddr)));

        updateKeysList();
        updateFromAddresses();
        return;
    }
#endif

    // Fallback when no wallet is available: generate a standalone SMSG key.
    // The private key is NOT in the wallet — the WIF shown below is the only copy.
    CKey key;
    key.MakeNewKey(true);

    int rv = smsgModule.ImportPrivkey(key, label.toStdString());
    if (rv != smsg::SMSG_NO_ERROR) {
        QMessageBox::warning(this, tr("Error"),
            tr("Failed to store key: %1").arg(QString::fromStdString(smsg::GetString(rv))));
        return;
    }

    CKeyID keyID = key.GetPubKey().GetID();
    std::string sAddr = EncodeDestination(PKHash(keyID));
    std::string sWIF  = EncodeSecret(key);

    QMessageBox msgBox(this);
    msgBox.setWindowTitle(tr("Key Generated (no wallet)"));
    msgBox.setText(tr("New SMSG key generated (standalone — no wallet attached).\n\n"
                      "Address:\n%1\n\n"
                      "Private key (WIF) — this is the ONLY copy, back it up now:\n%2")
        .arg(QString::fromStdString(sAddr))
        .arg(QString::fromStdString(sWIF)));
    msgBox.setStandardButtons(QMessageBox::Ok);
    msgBox.exec();

    updateKeysList();
    updateFromAddresses();
}

// ----- Trollbox -----

void MessagingPage::setupTrollboxTab()
{
    connect(ui->trollboxSendButton, &QPushButton::clicked, this, &MessagingPage::onTrollboxSendClicked);

    // Also send on Enter key
    connect(ui->trollboxInput, &QLineEdit::returnPressed, this, &MessagingPage::onTrollboxSendClicked);

    // Context menu on chat area
    ui->trollboxChat->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->trollboxChat, &QTextBrowser::customContextMenuRequested, this, &MessagingPage::showTrollboxContextMenu);

    trollboxContextMenu = new QMenu(this);
    trollboxContextMenu->addAction(tr("Copy Message"), this, &MessagingPage::copyTrollboxMessage);
    trollboxContextMenu->addAction(tr("Copy Sender Address"), this, &MessagingPage::copyTrollboxSender);
    trollboxContextMenu->addSeparator();
    trollboxContextMenu->addAction(tr("Mute Sender"), this, &MessagingPage::muteTrollboxSender);
}

void MessagingPage::updateTrollboxFromAddresses()
{
    if (!smsg::fSecMsgEnabled)
        return;

    QString current = ui->trollboxFromCombo->currentText();
    ui->trollboxFromCombo->clear();

    QSet<QString> seen;
    {
        LOCK(smsgModule.cs_smsg);

#ifdef ENABLE_WALLET
        if (smsgModule.pwallet) {
            auto* spk_man = smsgModule.pwallet->GetLegacyScriptPubKeyMan();
            for (auto it = smsgModule.addresses.begin(); it != smsgModule.addresses.end(); ++it) {
                if (!it->fReceiveEnabled)
                    continue;
                if (it->address == smsgModule.trollboxAddress)
                    continue;
                if (!spk_man || !spk_man->HaveKey(it->address))
                    continue;
                QString qAddr = QString::fromStdString(EncodeDestination(PKHash(it->address)));
                seen.insert(qAddr);
                ui->trollboxFromCombo->addItem(qAddr);
            }
        }
#endif

        for (auto& p : smsgModule.keyStore.mapKeys) {
            if (p.first == smsgModule.trollboxAddress)
                continue;
            auto& key = p.second;
            if (key.nFlags & smsg::SMK_CONTACT_ONLY)
                continue;
            if (!(key.nFlags & smsg::SMK_RECEIVE_ON))
                continue;
            if (!key.key.IsValid())
                continue;
            QString qAddr = QString::fromStdString(EncodeDestination(PKHash(p.first)));
            if (!seen.contains(qAddr)) {
                seen.insert(qAddr);
                ui->trollboxFromCombo->addItem(qAddr);
            }
        }
    }

    int idx = ui->trollboxFromCombo->findText(current);
    if (idx >= 0)
        ui->trollboxFromCombo->setCurrentIndex(idx);
}

void MessagingPage::updateTrollboxList()
{
    if (!smsg::fSecMsgEnabled)
        return;

    // Phase 1: key-only scan; decrypt only cache misses / prior failures.
    struct ToDecrypt {
        std::string keyStr;
        std::array<uint8_t, 30> chKey;
        smsg::SecMsgStored stored;
    };
    std::vector<ToDecrypt> toDecrypt;
    std::unordered_set<std::string> currentKeys;

    {
        LOCK(smsg::cs_smsgDB);
        smsg::SecMsgDB db;
        if (!db.Open("cr+"))
            return;

        uint8_t chKey[30];
        std::string sPrefix("tb");
        leveldb::Iterator* it = db.pdb->NewIterator(leveldb::ReadOptions());
        while (db.NextSmesgKey(it, sPrefix, chKey)) {
            std::string ks(reinterpret_cast<const char*>(chKey), 30);
            currentKeys.insert(ks);

            auto cit = m_trollboxCache.find(ks);
            if (cit != m_trollboxCache.end() && !cit->second.fDecryptFailed)
                continue; // valid cache hit

            smsg::SecMsgStored smsgStored;
            if (!db.ReadSmesg(chKey, smsgStored))
                continue;
            if (smsgStored.vchMessage.size() < smsg::SMSG_HDR_LEN)
                continue;

            ToDecrypt e;
            e.keyStr = ks;
            memcpy(e.chKey.data(), chKey, 30);
            e.stored = std::move(smsgStored);
            toDecrypt.push_back(std::move(e));
        }
        delete it;
    }

    // Phase 2: evict stale cache entries.
    for (auto it = m_trollboxCache.begin(); it != m_trollboxCache.end(); ) {
        if (!currentKeys.count(it->first))
            it = m_trollboxCache.erase(it);
        else
            ++it;
    }

    // Phase 3: decrypt new / failed entries.
    for (auto& e : toDecrypt) {
        uint8_t* pHeader = e.stored.vchMessage.data();
        const smsg::SecureMessage* psmsg = (smsg::SecureMessage*)pHeader;
        uint32_t nPayload = e.stored.vchMessage.size() - smsg::SMSG_HDR_LEN;

        TrollboxCached tc;
        memcpy(tc.chKey.data(), e.chKey.data(), 30);
        tc.fPaid = psmsg->IsPaidVersion();

        smsg::MessageData msg;
        int rv = smsgModule.Decrypt(false, e.stored.addrTo, pHeader, pHeader + smsg::SMSG_HDR_LEN, nPayload, msg);
        if (rv == 0) {
            tc.time          = msg.timestamp;
            tc.from          = QString::fromStdString(msg.sFromAddress);
            tc.text          = QString::fromStdString(std::string((char*)msg.vchMessage.data()));
            tc.fDecryptFailed = false;
        } else {
            tc.fDecryptFailed = true;
        }
        m_trollboxCache[e.keyStr] = std::move(tc);
    }

    // Phase 4: collect displayable messages (sorted by timestamp).
    struct TrollboxMsg { int64_t time; QString from; QString text; bool paid; };
    std::vector<TrollboxMsg> msgs;
    msgs.reserve(currentKeys.size());

    for (const auto& ks : currentKeys) {
        auto it = m_trollboxCache.find(ks);
        if (it == m_trollboxCache.end() || it->second.fDecryptFailed)
            continue;
        const TrollboxCached& tc = it->second;
        if (trollboxMuteList.count(tc.from.toStdString()))
            continue;
        msgs.push_back({tc.time, tc.from, tc.text, tc.fPaid});
    }

    std::sort(msgs.begin(), msgs.end(), [](const TrollboxMsg& a, const TrollboxMsg& b){
        return a.time < b.time;
    });

    if ((int)msgs.size() > smsg::TROLLBOX_MAX_DISPLAY)
        msgs.erase(msgs.begin(), msgs.begin() + (msgs.size() - smsg::TROLLBOX_MAX_DISPLAY));

    // Skip re-render if content unchanged.
    int64_t lastTs = msgs.empty() ? 0 : msgs.back().time;
    if (lastTs == m_trollboxLastTimestamp && msgs.size() == m_trollboxLastCount)
        return;
    m_trollboxLastTimestamp = lastTs;
    m_trollboxLastCount = msgs.size();

    QString html;
    for (const auto& m : msgs) {
        QString timeStr = QDateTime::fromSecsSinceEpoch(m.time).toString("hh:mm");
        QString senderShort = m.from;
        if (senderShort.length() > 12)
            senderShort = senderShort.left(6) + "..." + senderShort.right(4);
        QString escapedText = m.text.toHtmlEscaped().replace('\n', ' ');
        if (m.paid) {
            html += QString("<p style=\"margin:2px 0;\"><span style=\"color:gray;\">[%1]</span> "
                "<a href=\"addr:%4\" style=\"color:red;font-weight:bold;text-decoration:none;\">%2:</a> "
                "<span style=\"color:red;\">%3</span></p>")
                .arg(timeStr, senderShort, escapedText, m.from.toHtmlEscaped());
        } else {
            html += QString("<p style=\"margin:2px 0;\"><span style=\"color:gray;\">[%1]</span> "
                "<a href=\"addr:%4\" style=\"color:inherit;font-weight:bold;text-decoration:none;\">%2:</a> %3</p>")
                .arg(timeStr, senderShort, escapedText, m.from.toHtmlEscaped());
        }
    }

    QTextBrowser* chat = ui->trollboxChat;
    chat->setHtml(html);
    QTextCursor cursor = chat->textCursor();
    cursor.movePosition(QTextCursor::End);
    chat->setTextCursor(cursor);
    chat->ensureCursorVisible();
}

void MessagingPage::onTrollboxSendClicked()
{
    if (!smsg::fSecMsgEnabled) {
        ui->trollboxCooldownLabel->setText(tr("SMSG not enabled."));
        return;
    }

    if (nTrollboxCooldown > 0) {
        ui->trollboxCooldownLabel->setText(tr("Wait %1s...").arg(nTrollboxCooldown));
        return;
    }

    QString addrFrom = ui->trollboxFromCombo->currentText();
    QString message = ui->trollboxInput->text().trimmed();

    if (addrFrom.isEmpty()) {
        ui->trollboxCooldownLabel->setText(tr("Select a From address."));
        return;
    }
    if (message.isEmpty()) {
        return;
    }
    if ((unsigned int)message.toUtf8().size() > smsg::TROLLBOX_MAX_MSG_BYTES) {
        ui->trollboxCooldownLabel->setText(tr("Message too long (max %1 chars).").arg(smsg::TROLLBOX_MAX_MSG_BYTES));
        return;
    }

    CTxDestination destFrom = DecodeDestination(addrFrom.toStdString());
    if (!IsValidDestination(destFrom)) {
        ui->trollboxCooldownLabel->setText(tr("Invalid From address."));
        return;
    }

    const PKHash *pkhFrom = std::get_if<PKHash>(&destFrom);
    if (!pkhFrom) {
        ui->trollboxCooldownLabel->setText(tr("SMSG requires a P2PKH address."));
        return;
    }
    CKeyID kiFrom = ToKeyID(*pkhFrom);
    CKeyID kiTo = smsgModule.trollboxAddress;

    std::string sMessage = message.toStdString();
    std::string sError;
    smsg::SecureMessage smsgOut;
    bool fPaid = ui->trollboxPaidCheckBox->isChecked();

    if (fPaid) {
        // Estimate fee first, confirm with user
        CAmount nFee = 0;
        int rv = smsgModule.Send(kiFrom, kiTo, sMessage, smsgOut, sError, true, 1, true, &nFee);
        if (rv != 0) {
            ui->trollboxCooldownLabel->setText(tr("Fee estimate failed: %1").arg(QString::fromStdString(sError)));
            return;
        }

        QMessageBox::StandardButton reply = QMessageBox::question(this, tr("Paid Trollbox Message"),
            tr("Send paid message (red highlight)?\nFee: %1 OMEGA")
                .arg(QString::number((double)nFee / 100000000.0, 'f', 8)),
            QMessageBox::Yes | QMessageBox::No);

        if (reply != QMessageBox::Yes)
            return;
    }

    int rv = smsgModule.Send(kiFrom, kiTo, sMessage, smsgOut, sError, fPaid, fPaid ? 1 : 0);

    if (rv != 0) {
        ui->trollboxCooldownLabel->setText(tr("Send failed: %1").arg(QString::fromStdString(sError)));
        return;
    }

    ui->trollboxInput->clear();
    ui->trollboxPaidCheckBox->setChecked(false);

    // Start cooldown
    nTrollboxCooldown = smsg::TROLLBOX_RATE_LIMIT_SECS;
    ui->trollboxSendButton->setEnabled(false);
    ui->trollboxCooldownLabel->setText(tr("Wait %1s...").arg(nTrollboxCooldown));
    trollboxCooldownTimer->start();

    // Trigger immediate refresh
    fTrollboxChanged = true;
}

void MessagingPage::onTrollboxCooldownTick()
{
    nTrollboxCooldown--;
    if (nTrollboxCooldown <= 0) {
        nTrollboxCooldown = 0;
        trollboxCooldownTimer->stop();
        ui->trollboxSendButton->setEnabled(true);
        ui->trollboxCooldownLabel->setText("");
    } else {
        ui->trollboxCooldownLabel->setText(tr("Wait %1s...").arg(nTrollboxCooldown));
    }
}

void MessagingPage::showTrollboxContextMenu(const QPoint& point)
{
    // Find which message line was clicked
    QTextCursor cursor = ui->trollboxChat->cursorForPosition(point);
    cursor.select(QTextCursor::BlockUnderCursor);
    QString block = cursor.selectedText();

    // Parse sender and message from the block
    // Format: [HH:MM] Sender: message
    trollboxSelectedSender.clear();
    trollboxSelectedMessage.clear();

    // Extract message text from visible block
    int bracketEnd = block.indexOf("] ");
    int colonPos = block.indexOf(":", bracketEnd > 0 ? bracketEnd : 0);
    if (bracketEnd > 0 && colonPos > bracketEnd) {
        trollboxSelectedMessage = block.mid(colonPos + 1).trimmed();
    }

    // Extract full sender address from the anchor href embedded in the HTML
    QString anchor = ui->trollboxChat->anchorAt(point);
    if (anchor.startsWith("addr:")) {
        trollboxSelectedSender = anchor.mid(5);
    }

    if (!trollboxSelectedSender.isEmpty()) {
        trollboxContextMenu->exec(ui->trollboxChat->mapToGlobal(point));
    }
}

void MessagingPage::copyTrollboxMessage()
{
    if (!trollboxSelectedMessage.isEmpty())
        QApplication::clipboard()->setText(trollboxSelectedMessage);
}

void MessagingPage::copyTrollboxSender()
{
    if (!trollboxSelectedSender.isEmpty())
        QApplication::clipboard()->setText(trollboxSelectedSender);
}

void MessagingPage::muteTrollboxSender()
{
    if (trollboxSelectedSender.isEmpty())
        return;

    QMessageBox::StandardButton reply = QMessageBox::question(this, tr("Mute Sender"),
        tr("Mute messages from %1?").arg(trollboxSelectedSender),
        QMessageBox::Yes | QMessageBox::No);

    if (reply != QMessageBox::Yes)
        return;

    trollboxMuteList.insert(trollboxSelectedSender.toStdString());

    updateTrollboxList();
}

// ----- Filter handlers -----

void MessagingPage::filterInbox(const QString& /*text*/)
{
    updateInboxList();
}

void MessagingPage::filterOutbox(const QString& /*text*/)
{
    updateOutboxList();
}

void MessagingPage::filterKeys(const QString& /*text*/)
{
    updateKeysList();
}
