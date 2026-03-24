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
    fInboxChanged(false),
    fOutboxChanged(false),
    fTrollboxChanged(false),
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
        updateInboxList();
        updateOutboxList();
        updateKeysList();
        updateFromAddresses();
        updateTrollboxFromAddresses();
        updateTrollboxList();
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

    QTableWidget* table = ui->inboxTable;
    table->setSortingEnabled(false);
    table->setRowCount(0);

    QString filterText = ui->inboxFilterLineEdit->text();
    int nMessages = 0;

    {
        LOCK(smsg::cs_smsgDB);
        smsg::SecMsgDB dbInbox;
        if (!dbInbox.Open("cr+"))
            return;

        std::string sPrefix("im");
        uint8_t chKey[30];

        smsg::SecMsgStored smsgStored;
        smsg::MessageData msg;

        leveldb::Iterator* it = dbInbox.pdb->NewIterator(leveldb::ReadOptions());
        while (dbInbox.NextSmesg(it, sPrefix, chKey, smsgStored))
        {
            uint8_t* pHeader = &smsgStored.vchMessage[0];
            const smsg::SecureMessage* psmsg = (smsg::SecureMessage*)pHeader;

            QString sMsgId = QString::fromStdString(HexStr(Span<uint8_t>(&chKey[2], &chKey[2] + 28)));
            bool fUnread = (smsgStored.status & SMSG_MASK_UNREAD) != 0;

            uint32_t nPayload = smsgStored.vchMessage.size() - smsg::SMSG_HDR_LEN;
            int rv = smsgModule.Decrypt(false, smsgStored.addrTo, pHeader, pHeader + smsg::SMSG_HDR_LEN, nPayload, msg);

            QString sFrom, sTo, sText;
            int64_t timeSent = 0;
            bool fPaid = psmsg->IsPaidVersion();
            uint32_t nDaysRetention = fPaid ? psmsg->nonce[0] : 2;

            if (rv == 0) {
                sFrom = QString::fromStdString(msg.sFromAddress);
                sTo = QString::fromStdString(EncodeDestination(PKHash(smsgStored.addrTo)));
                sText = QString::fromStdString(std::string((char*)msg.vchMessage.data()));
                timeSent = msg.timestamp;
            } else {
                sText = tr("[Decrypt failed: %1]").arg(QString::fromStdString(smsg::GetString(rv)));
            }

            // Apply filter
            if (!filterText.isEmpty()) {
                if (!sFrom.contains(filterText, Qt::CaseInsensitive) &&
                    !sTo.contains(filterText, Qt::CaseInsensitive) &&
                    !sText.contains(filterText, Qt::CaseInsensitive))
                    continue;
            }

            int row = table->rowCount();
            table->insertRow(row);

            // Read status
            QTableWidgetItem* readItem = new QTableWidgetItem(fUnread ? tr("*") : QString());
            readItem->setTextAlignment(Qt::AlignCenter);
            table->setItem(row, INBOX_COL_READ, readItem);

            // Date received
            QTableWidgetItem* recvItem = new QTableWidgetItem(
                QDateTime::fromSecsSinceEpoch(smsgStored.timeReceived).toString("yyyy-MM-dd hh:mm"));
            recvItem->setData(Qt::UserRole, (qlonglong)smsgStored.timeReceived);
            table->setItem(row, INBOX_COL_DATE_RECV, recvItem);

            // Date sent
            QTableWidgetItem* sentItem = new QTableWidgetItem(
                timeSent > 0 ? QDateTime::fromSecsSinceEpoch(timeSent).toString("yyyy-MM-dd hh:mm") : QString());
            sentItem->setData(Qt::UserRole, (qlonglong)timeSent);
            table->setItem(row, INBOX_COL_DATE_SENT, sentItem);

            table->setItem(row, INBOX_COL_FROM, new QTableWidgetItem(sFrom));
            table->setItem(row, INBOX_COL_TO, new QTableWidgetItem(sTo));
            table->setItem(row, INBOX_COL_PAID, new QTableWidgetItem(fPaid ? tr("Yes") : tr("No")));

            QTableWidgetItem* retItem = new QTableWidgetItem(QString::number(nDaysRetention));
            retItem->setTextAlignment(Qt::AlignCenter);
            table->setItem(row, INBOX_COL_RETENTION, retItem);

            // Text preview (first 80 chars), full text in UserRole
            QString preview = sText.left(80).replace('\n', ' ');
            QTableWidgetItem* textItem = new QTableWidgetItem(preview);
            textItem->setData(Qt::UserRole, sText);
            table->setItem(row, INBOX_COL_TEXT, textItem);

            table->setItem(row, INBOX_COL_MSGID, new QTableWidgetItem(sMsgId));

            // Bold unread rows
            if (fUnread) {
                QFont font = table->font();
                font.setBold(true);
                for (int col = 0; col < INBOX_COL_COUNT; ++col) {
                    QTableWidgetItem* item = table->item(row, col);
                    if (item) item->setFont(font);
                }
            }

            nMessages++;
        }
        delete it;
    } // cs_smsgDB

    table->setSortingEnabled(true);
    ui->inboxCountLabel->setText(QString::number(nMessages) + tr(" messages"));
}

void MessagingPage::updateOutboxList()
{
    if (!smsg::fSecMsgEnabled)
        return;

    QTableWidget* table = ui->outboxTable;
    table->setSortingEnabled(false);
    table->setRowCount(0);

    QString filterText = ui->outboxFilterLineEdit->text();
    int nMessages = 0;

    {
        LOCK(smsg::cs_smsgDB);
        smsg::SecMsgDB dbOutbox;
        if (!dbOutbox.Open("cr+"))
            return;

        std::string sPrefix("sm");
        uint8_t chKey[30];

        smsg::SecMsgStored smsgStored;
        smsg::MessageData msg;

        leveldb::Iterator* it = dbOutbox.pdb->NewIterator(leveldb::ReadOptions());
        while (dbOutbox.NextSmesg(it, sPrefix, chKey, smsgStored))
        {
            uint8_t* pHeader = &smsgStored.vchMessage[0];
            const smsg::SecureMessage* psmsg = (smsg::SecureMessage*)pHeader;

            QString sMsgId = QString::fromStdString(HexStr(Span<uint8_t>(&chKey[2], &chKey[2] + 28)));

            uint32_t nPayload = smsgStored.vchMessage.size() - smsg::SMSG_HDR_LEN;
            int rv = smsgModule.Decrypt(false, smsgStored.addrOutbox, pHeader, pHeader + smsg::SMSG_HDR_LEN, nPayload, msg);

            QString sFrom, sTo, sText;
            int64_t timeSent = 0;
            bool fPaid = psmsg->IsPaidVersion();
            uint32_t nDaysRetention = fPaid ? psmsg->nonce[0] : 2;

            if (rv == 0) {
                sFrom = QString::fromStdString(msg.sFromAddress);
                sTo = QString::fromStdString(EncodeDestination(PKHash(smsgStored.addrTo)));
                sText = QString::fromStdString(std::string((char*)msg.vchMessage.data()));
                timeSent = msg.timestamp;
            } else {
                sText = tr("[Decrypt failed: %1]").arg(QString::fromStdString(smsg::GetString(rv)));
            }

            // Apply filter
            if (!filterText.isEmpty()) {
                if (!sFrom.contains(filterText, Qt::CaseInsensitive) &&
                    !sTo.contains(filterText, Qt::CaseInsensitive) &&
                    !sText.contains(filterText, Qt::CaseInsensitive))
                    continue;
            }

            int row = table->rowCount();
            table->insertRow(row);

            QTableWidgetItem* sentItem = new QTableWidgetItem(
                timeSent > 0 ? QDateTime::fromSecsSinceEpoch(timeSent).toString("yyyy-MM-dd hh:mm") : QString());
            sentItem->setData(Qt::UserRole, (qlonglong)timeSent);
            table->setItem(row, OUTBOX_COL_DATE_SENT, sentItem);

            table->setItem(row, OUTBOX_COL_FROM, new QTableWidgetItem(sFrom));
            table->setItem(row, OUTBOX_COL_TO, new QTableWidgetItem(sTo));
            table->setItem(row, OUTBOX_COL_PAID, new QTableWidgetItem(fPaid ? tr("Yes") : tr("No")));

            QTableWidgetItem* retItem = new QTableWidgetItem(QString::number(nDaysRetention));
            retItem->setTextAlignment(Qt::AlignCenter);
            table->setItem(row, OUTBOX_COL_RETENTION, retItem);

            // Text preview (first 80 chars), full text in UserRole
            QString preview = sText.left(80).replace('\n', ' ');
            QTableWidgetItem* textItem = new QTableWidgetItem(preview);
            textItem->setData(Qt::UserRole, sText);
            table->setItem(row, OUTBOX_COL_TEXT, textItem);

            table->setItem(row, OUTBOX_COL_MSGID, new QTableWidgetItem(sMsgId));

            nMessages++;
        }
        delete it;
    } // cs_smsgDB

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

    {
        LOCK(smsgModule.cs_smsg);

#ifdef ENABLE_WALLET
        if (smsgModule.pwallet) {
            for (auto it = smsgModule.addresses.begin(); it != smsgModule.addresses.end(); ++it)
            {
                QString sAddr = QString::fromStdString(EncodeDestination(PKHash(it->address)));
                PKHash pkh = PKHash(it->address);
                QString sLabel = QString::fromStdString(smsgModule.LookupLabel(pkh));
                bool fRecv = it->fReceiveEnabled;
                bool fAnon = it->fReceiveAnon;

                if (!filterText.isEmpty()) {
                    if (!sAddr.contains(filterText, Qt::CaseInsensitive) &&
                        !sLabel.contains(filterText, Qt::CaseInsensitive))
                        continue;
                }

                // Get pubkey for "Copy Public Key"
                QString sPubKey;
                {
                    LOCK(smsgModule.pwallet->cs_wallet);
                    auto spk = smsgModule.pwallet->GetLegacyScriptPubKeyMan();
                    if (spk) {
                        CPubKey pubKey;
                        if (spk->GetPubKey(CKeyID(it->address), pubKey) && pubKey.IsValid())
                            sPubKey = QString::fromStdString(HexStr(pubKey));
                    }
                }

                int row = table->rowCount();
                table->insertRow(row);
                QTableWidgetItem* addrItem = new QTableWidgetItem(sAddr);
                addrItem->setData(Qt::UserRole, sPubKey);
                table->setItem(row, KEYS_COL_ADDRESS, addrItem);
                table->setItem(row, KEYS_COL_LABEL, new QTableWidgetItem(sLabel));
                table->setItem(row, KEYS_COL_RECEIVE, new QTableWidgetItem(fRecv ? tr("On") : tr("Off")));
                table->setItem(row, KEYS_COL_ANON, new QTableWidgetItem(fAnon ? tr("On") : tr("Off")));
                table->setItem(row, KEYS_COL_SOURCE, new QTableWidgetItem(tr("My Key")));
                // Highlight own keys so they are visually distinct from contacts
                QColor ownKeyColor(210, 240, 210); // light green
                for (int col = 0; col < KEYS_COL_COUNT; ++col) {
                    if (table->item(row, col))
                        table->item(row, col)->setBackground(QBrush(ownKeyColor));
                }
            }
        }
#endif

        for (auto& p : smsgModule.keyStore.mapKeys)
        {
            // Skip Trollbox shared key — not a user key
            if (p.first == smsgModule.trollboxAddress)
                continue;
            auto& key = p.second;
            bool fContact = (key.nFlags & smsg::SMK_CONTACT_ONLY) != 0;
            QString sAddr = QString::fromStdString(EncodeDestination(PKHash(p.first)));
            QString sLabel = QString::fromStdString(key.sLabel);
            bool fRecv = (key.nFlags & smsg::SMK_RECEIVE_ON) != 0;
            bool fAnon = (key.nFlags & smsg::SMK_RECEIVE_ANON) != 0;

            if (!filterText.isEmpty()) {
                if (!sAddr.contains(filterText, Qt::CaseInsensitive) &&
                    !sLabel.contains(filterText, Qt::CaseInsensitive))
                    continue;
            }

            // Get pubkey for "Copy Public Key"
            QString sPubKey;
            if (fContact) {
                // Contact: pubkey is in addrpkdb
                LOCK(smsg::cs_smsgDB);
                smsg::SecMsgDB db;
                if (db.Open("cr+")) {
                    CPubKey pubKey;
                    if (db.ReadPK(p.first, pubKey) && pubKey.IsValid())
                        sPubKey = QString::fromStdString(HexStr(pubKey));
                }
            } else if (key.key.IsValid()) {
                CPubKey pubKey = key.key.GetPubKey();
                if (pubKey.IsValid())
                    sPubKey = QString::fromStdString(HexStr(pubKey));
            }

            int row = table->rowCount();
            table->insertRow(row);
            QTableWidgetItem* addrItem = new QTableWidgetItem(sAddr);
            addrItem->setData(Qt::UserRole, sPubKey);
            table->setItem(row, KEYS_COL_ADDRESS, addrItem);
            table->setItem(row, KEYS_COL_LABEL, new QTableWidgetItem(sLabel));
            if (fContact) {
                table->setItem(row, KEYS_COL_RECEIVE, new QTableWidgetItem(tr("N/A")));
                table->setItem(row, KEYS_COL_ANON, new QTableWidgetItem(tr("N/A")));
                table->setItem(row, KEYS_COL_SOURCE, new QTableWidgetItem(tr("Contact")));
            } else {
                table->setItem(row, KEYS_COL_RECEIVE, new QTableWidgetItem(fRecv ? tr("On") : tr("Off")));
                table->setItem(row, KEYS_COL_ANON, new QTableWidgetItem(fAnon ? tr("On") : tr("Off")));
                table->setItem(row, KEYS_COL_SOURCE, new QTableWidgetItem(tr("Imported Key")));
            }
        }
    } // cs_smsg
}

void MessagingPage::updateFromAddresses()
{
    if (!smsg::fSecMsgEnabled)
        return;

    QString current = ui->fromAddressCombo->currentText();
    ui->fromAddressCombo->clear();

    {
        LOCK(smsgModule.cs_smsg);

#ifdef ENABLE_WALLET
        if (smsgModule.pwallet) {
            auto* spk_man = smsgModule.pwallet->GetLegacyScriptPubKeyMan();
            for (auto it = smsgModule.addresses.begin(); it != smsgModule.addresses.end(); ++it)
            {
                if (!it->fReceiveEnabled)
                    continue;
                // Only show addresses whose private key is in the wallet
                if (!spk_man || !spk_man->HaveKey(it->address))
                    continue;
                std::string sAddr = EncodeDestination(PKHash(it->address));
                ui->fromAddressCombo->addItem(QString::fromStdString(sAddr));
            }
        }
#endif

        // Also include standalone SMSG keys (keyStore) that have receive enabled
        // and are not contact-only. These are used when no wallet is attached or
        // when a key was generated via ImportPrivkey (e.g. unencrypted wallet path).
        // Skip the Trollbox address — it's a shared key, not a user address.
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
            std::string sAddr = EncodeDestination(PKHash(p.first));
            QString qAddr = QString::fromStdString(sAddr);
            if (ui->fromAddressCombo->findText(qAddr) < 0)
                ui->fromAddressCombo->addItem(qAddr);
        }
    }

    // Restore previous selection if possible
    int idx = ui->fromAddressCombo->findText(current);
    if (idx >= 0) {
        ui->fromAddressCombo->setCurrentIndex(idx);
    }
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

    CKeyID kiFrom, kiTo;
    CTxDestination destFrom = DecodeDestination(addrFrom.toStdString());
    CTxDestination destTo = DecodeDestination(addrTo.toStdString());

    if (!IsValidDestination(destFrom) || !IsValidDestination(destTo)) {
        ui->feeEstimateLabel->setText(tr("Invalid address"));
        return;
    }

    kiFrom = ToKeyID(std::get<PKHash>(destFrom));
    kiTo = ToKeyID(std::get<PKHash>(destTo));

    std::string message = ui->messageEdit->toPlainText().toStdString();
    if (message.empty()) message = "x"; // need non-empty for fee estimate

    std::string sError;
    smsg::SecureMessage smsgOut;
    CAmount nFee = 0;
    int nRetention = ui->retentionSpinBox->value();

    int rv = smsgModule.Send(kiFrom, kiTo, message, smsgOut, sError,
        true, nRetention, true, &nFee);

    if (rv == 0) {
        ui->feeEstimateLabel->setText(tr("Estimated fee: %1 OMEGA").arg(
            QString::number((double)nFee / 100000000.0, 'f', 8)));
    } else {
        ui->feeEstimateLabel->setText(tr("Fee estimate failed"));
    }
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

    CKeyID kiFrom = ToKeyID(std::get<PKHash>(destFrom));
    CKeyID kiTo = ToKeyID(std::get<PKHash>(destTo));

    std::string sMessage = message.toStdString();
    std::string sError;
    smsg::SecureMessage smsgOut;
    CAmount nFee = 0;
    int nRetention = ui->retentionSpinBox->value();

    if (fPaid) {
        // Confirm paid message
        int rv = smsgModule.Send(kiFrom, kiTo, sMessage, smsgOut, sError, true, nRetention, true, &nFee);
        if (rv != 0) {
            ui->sendStatusLabel->setText(tr("Send failed: %1").arg(QString::fromStdString(sError)));
            return;
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
        bool isMyKey = sourceItem && sourceItem->text() == tr("My Key");
        sendMessageAction->setVisible(!isMyKey);
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
    QTableWidgetItem* msgIdItem = ui->inboxTable->item(row, INBOX_COL_MSGID);
    if (!msgIdItem) return;

    std::string sMsgId = msgIdItem->text().toStdString();

    {
        LOCK(smsg::cs_smsgDB);
        smsg::SecMsgDB dbInbox;
        if (!dbInbox.Open("cr+"))
            return;

        std::string sPrefix("im");
        uint8_t chKey[30];
        smsg::SecMsgStored smsgStored;

        dbInbox.TxnBegin();
        leveldb::Iterator* it = dbInbox.pdb->NewIterator(leveldb::ReadOptions());
        while (dbInbox.NextSmesg(it, sPrefix, chKey, smsgStored))
        {
            std::string keyMsgId = HexStr(Span<uint8_t>(&chKey[2], &chKey[2] + 28));
            if (keyMsgId == sMsgId) {
                smsgStored.status &= ~SMSG_MASK_UNREAD;
                dbInbox.WriteSmesg(chKey, smsgStored);
                break;
            }
        }
        delete it;
        dbInbox.TxnCommit();
    }
    updateInboxList();
}

void MessagingPage::markUnread()
{
    int row = ui->inboxTable->currentRow();
    if (row < 0) return;
    QTableWidgetItem* msgIdItem = ui->inboxTable->item(row, INBOX_COL_MSGID);
    if (!msgIdItem) return;

    std::string sMsgId = msgIdItem->text().toStdString();

    {
        LOCK(smsg::cs_smsgDB);
        smsg::SecMsgDB dbInbox;
        if (!dbInbox.Open("cr+"))
            return;

        std::string sPrefix("im");
        uint8_t chKey[30];
        smsg::SecMsgStored smsgStored;

        dbInbox.TxnBegin();
        leveldb::Iterator* it = dbInbox.pdb->NewIterator(leveldb::ReadOptions());
        while (dbInbox.NextSmesg(it, sPrefix, chKey, smsgStored))
        {
            std::string keyMsgId = HexStr(Span<uint8_t>(&chKey[2], &chKey[2] + 28));
            if (keyMsgId == sMsgId) {
                smsgStored.status |= SMSG_MASK_UNREAD;
                dbInbox.WriteSmesg(chKey, smsgStored);
                break;
            }
        }
        delete it;
        dbInbox.TxnCommit();
    }
    updateInboxList();
}

void MessagingPage::deleteSelectedInbox()
{
    int row = ui->inboxTable->currentRow();
    if (row < 0) return;
    QTableWidgetItem* msgIdItem = ui->inboxTable->item(row, INBOX_COL_MSGID);
    if (!msgIdItem) return;

    std::string sMsgId = msgIdItem->text().toStdString();

    {
        LOCK(smsg::cs_smsgDB);
        smsg::SecMsgDB dbInbox;
        if (!dbInbox.Open("cr+"))
            return;

        std::string sPrefix("im");
        uint8_t chKey[30];
        smsg::SecMsgStored smsgStored;

        dbInbox.TxnBegin();
        leveldb::Iterator* it = dbInbox.pdb->NewIterator(leveldb::ReadOptions());
        while (dbInbox.NextSmesg(it, sPrefix, chKey, smsgStored))
        {
            std::string keyMsgId = HexStr(Span<uint8_t>(&chKey[2], &chKey[2] + 28));
            if (keyMsgId == sMsgId) {
                dbInbox.EraseSmesg(chKey);
                break;
            }
        }
        delete it;
        dbInbox.TxnCommit();
    }
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
    QTableWidgetItem* msgIdItem = ui->outboxTable->item(row, OUTBOX_COL_MSGID);
    if (!msgIdItem) return;

    std::string sMsgId = msgIdItem->text().toStdString();

    {
        LOCK(smsg::cs_smsgDB);
        smsg::SecMsgDB dbOutbox;
        if (!dbOutbox.Open("cr+"))
            return;

        std::string sPrefix("sm");
        uint8_t chKey[30];
        smsg::SecMsgStored smsgStored;

        dbOutbox.TxnBegin();
        leveldb::Iterator* it = dbOutbox.pdb->NewIterator(leveldb::ReadOptions());
        while (dbOutbox.NextSmesg(it, sPrefix, chKey, smsgStored))
        {
            std::string keyMsgId = HexStr(Span<uint8_t>(&chKey[2], &chKey[2] + 28));
            if (keyMsgId == sMsgId) {
                dbOutbox.EraseSmesg(chKey);
                break;
            }
        }
        delete it;
        dbOutbox.TxnCommit();
    }
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
    if (!IsValidDestination(dest)) return;
    CKeyID keyID = ToKeyID(std::get<PKHash>(dest));

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
    if (!IsValidDestination(dest)) return;
    CKeyID keyID = ToKeyID(std::get<PKHash>(dest));

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

void MessagingPage::onScanChainClicked()
{
    if (!smsg::fSecMsgEnabled) {
        QMessageBox::warning(this, tr("Messaging"), tr("Secure messaging is not enabled."));
        return;
    }

    ui->scanChainButton->setEnabled(false);
    ui->scanChainButton->setText(tr("Scanning..."));

    QApplication::processEvents();

    bool fResult = smsgModule.ScanBlockChain();

    ui->scanChainButton->setEnabled(true);
    ui->scanChainButton->setText(tr("Scan Blockchain"));

    if (fResult) {
        QMessageBox::information(this, tr("Scan Complete"), tr("Blockchain scan completed successfully."));
    } else {
        QMessageBox::warning(this, tr("Scan Failed"), tr("Blockchain scan failed."));
    }

    updateKeysList();
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

    QString pubkey = QInputDialog::getText(this, tr("Add Contact"),
        tr("Enter public key (hex):"), QLineEdit::Normal, QString(), &ok);
    if (!ok || pubkey.isEmpty())
        return;

    QString label = QInputDialog::getText(this, tr("Add Contact"),
        tr("Label:"), QLineEdit::Normal, QString(), &ok);
    if (!ok || label.isEmpty())
        return;

    std::string sAddr = address.toStdString();
    std::string sPubKey = pubkey.toStdString();
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

    {
        LOCK(smsgModule.cs_smsg);

#ifdef ENABLE_WALLET
        if (smsgModule.pwallet) {
            auto* spk_man = smsgModule.pwallet->GetLegacyScriptPubKeyMan();
            for (auto it = smsgModule.addresses.begin(); it != smsgModule.addresses.end(); ++it)
            {
                if (!it->fReceiveEnabled)
                    continue;
                // Skip the Trollbox address itself
                if (it->address == smsgModule.trollboxAddress)
                    continue;
                // Only show addresses whose private key is in the wallet
                if (!spk_man || !spk_man->HaveKey(it->address))
                    continue;
                std::string sAddr = EncodeDestination(PKHash(it->address));
                ui->trollboxFromCombo->addItem(QString::fromStdString(sAddr));
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
            std::string sAddr = EncodeDestination(PKHash(p.first));
            QString qAddr = QString::fromStdString(sAddr);
            if (ui->trollboxFromCombo->findText(qAddr) < 0)
                ui->trollboxFromCombo->addItem(qAddr);
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

    QTextBrowser* chat = ui->trollboxChat;
    chat->clear();

    struct TrollboxMsg {
        int64_t time;
        QString from;
        QString text;
        bool paid;
    };
    std::vector<TrollboxMsg> msgs;

    {
        LOCK(smsg::cs_smsgDB);
        smsg::SecMsgDB db;
        if (!db.Open("cr+"))
            return;

        std::string sPrefix("tb");
        uint8_t chKey[30];

        smsg::SecMsgStored smsgStored;
        smsg::MessageData msg;

        leveldb::Iterator* it = db.pdb->NewIterator(leveldb::ReadOptions());
        while (db.NextSmesg(it, sPrefix, chKey, smsgStored))
        {
            uint8_t* pHeader = &smsgStored.vchMessage[0];
            const smsg::SecureMessage* psmsg = (smsg::SecureMessage*)pHeader;

            uint32_t nPayload = smsgStored.vchMessage.size() - smsg::SMSG_HDR_LEN;
            int rv = smsgModule.Decrypt(false, smsgStored.addrTo, pHeader, pHeader + smsg::SMSG_HDR_LEN, nPayload, msg);

            if (rv != 0)
                continue;

            // Skip muted senders
            if (trollboxMuteList.count(msg.sFromAddress))
                continue;

            TrollboxMsg tm;
            tm.time = msg.timestamp;
            tm.from = QString::fromStdString(msg.sFromAddress);
            tm.text = QString::fromStdString(std::string((char*)msg.vchMessage.data()));
            tm.paid = psmsg->IsPaidVersion();
            msgs.push_back(tm);
        }
        delete it;
    } // cs_smsgDB

    // Keep only the most recent messages
    if ((int)msgs.size() > smsg::TROLLBOX_MAX_DISPLAY) {
        msgs.erase(msgs.begin(), msgs.begin() + (msgs.size() - smsg::TROLLBOX_MAX_DISPLAY));
    }

    // Render messages as HTML
    QString html;
    for (const auto& m : msgs) {
        QString timeStr = QDateTime::fromSecsSinceEpoch(m.time).toString("hh:mm");
        QString senderShort = m.from;
        if (senderShort.length() > 12)
            senderShort = senderShort.left(6) + "..." + senderShort.right(4);

        // Escape HTML in message text
        QString escapedText = m.text.toHtmlEscaped().replace('\n', ' ');

        if (m.paid) {
            html += QString("<p style=\"margin:2px 0;\"><span style=\"color:gray;\">[%1]</span> "
                "<b style=\"color:red;\">%2:</b> <span style=\"color:red;\">%3</span></p>")
                .arg(timeStr, senderShort, escapedText);
        } else {
            html += QString("<p style=\"margin:2px 0;\"><span style=\"color:gray;\">[%1]</span> "
                "<b>%2:</b> %3</p>")
                .arg(timeStr, senderShort, escapedText);
        }
    }

    chat->setHtml(html);

    // Auto-scroll to bottom
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

    CKeyID kiFrom = ToKeyID(std::get<PKHash>(destFrom));
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

    // Extract text between first "] " and ":" for sender
    int bracketEnd = block.indexOf("] ");
    int colonPos = block.indexOf(":", bracketEnd > 0 ? bracketEnd : 0);
    if (bracketEnd > 0 && colonPos > bracketEnd) {
        trollboxSelectedSender = block.mid(bracketEnd + 2, colonPos - bracketEnd - 2).trimmed();
        trollboxSelectedMessage = block.mid(colonPos + 1).trimmed();
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

    // The displayed sender might be truncated — need to find full address
    // For now, store truncated version and match on prefix+suffix in updateTrollboxList
    QMessageBox::StandardButton reply = QMessageBox::question(this, tr("Mute Sender"),
        tr("Mute messages from %1?").arg(trollboxSelectedSender),
        QMessageBox::Yes | QMessageBox::No);

    if (reply != QMessageBox::Yes)
        return;

    // Need to find the full address — scan trollbox messages for a match
    {
        LOCK(smsg::cs_smsgDB);
        smsg::SecMsgDB db;
        if (!db.Open("cr+"))
            return;

        std::string sPrefix("tb");
        uint8_t chKey[30];
        smsg::SecMsgStored smsgStored;
        smsg::MessageData msg;

        leveldb::Iterator* it = db.pdb->NewIterator(leveldb::ReadOptions());
        while (db.NextSmesg(it, sPrefix, chKey, smsgStored))
        {
            uint8_t* pHeader = &smsgStored.vchMessage[0];
            uint32_t nPayload = smsgStored.vchMessage.size() - smsg::SMSG_HDR_LEN;
            int rv = smsgModule.Decrypt(false, smsgStored.addrTo, pHeader, pHeader + smsg::SMSG_HDR_LEN, nPayload, msg);
            if (rv != 0) continue;

            QString fullAddr = QString::fromStdString(msg.sFromAddress);
            QString shortAddr = fullAddr;
            if (shortAddr.length() > 12)
                shortAddr = shortAddr.left(6) + "..." + shortAddr.right(4);

            if (shortAddr == trollboxSelectedSender) {
                trollboxMuteList.insert(msg.sFromAddress);
                break;
            }
        }
        delete it;
    }

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
