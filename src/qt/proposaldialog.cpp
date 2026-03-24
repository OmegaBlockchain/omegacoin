// Copyright (c) 2026 The Omega Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/proposaldialog.h>
#include <qt/forms/ui_proposaldialog.h>

#include <qt/rpcconsole.h>
#include <qt/walletmodel.h>
#include <util/strencodings.h>

#include <univalue.h>

#include <QDateTime>
#include <QMessageBox>

ProposalDialog::ProposalDialog(WalletModel* _walletModel, QWidget* parent) :
    QDialog(parent),
    ui(new Ui::ProposalDialog),
    walletModel(_walletModel),
    confirmTimer(new QTimer(this))
{
    ui->setupUi(this);

    connect(ui->buttonPrepare, &QPushButton::clicked, this, &ProposalDialog::onPrepareClicked);
    connect(ui->buttonSubmit, &QPushButton::clicked, this, &ProposalDialog::onSubmitClicked);
    connect(ui->buttonClose, &QPushButton::clicked, this, &QDialog::reject);
    connect(confirmTimer, &QTimer::timeout, this, &ProposalDialog::checkConfirmations);
}

ProposalDialog::~ProposalDialog()
{
    delete ui;
}

bool ProposalDialog::executeRpc(const std::string& command, std::string& result)
{
    if (!walletModel) {
        result = "Wallet model not available.";
        return false;
    }
    try {
        return RPCConsole::RPCExecuteCommandLine(walletModel->node(), result, command, nullptr, walletModel);
    } catch (UniValue& objError) {
        try {
            result = find_value(objError, "message").get_str();
        } catch (const std::runtime_error&) {
            result = objError.write();
        }
        return false;
    } catch (const std::exception& e) {
        result = e.what();
        return false;
    }
}

void ProposalDialog::setFormEnabled(bool enabled)
{
    ui->editName->setEnabled(enabled);
    ui->editUrl->setEnabled(enabled);
    ui->spinPayments->setEnabled(enabled);
    ui->editAmount->setEnabled(enabled);
    ui->editAddress->setEnabled(enabled);
    ui->buttonPrepare->setEnabled(enabled);
}

void ProposalDialog::onPrepareClicked()
{
    // Validate fields.
    QString name = ui->editName->text().trimmed().toLower();
    QString url = ui->editUrl->text().trimmed();
    int payments = ui->spinPayments->value();
    QString amountStr = ui->editAmount->text().trimmed();
    QString address = ui->editAddress->text().trimmed();

    if (name.isEmpty()) {
        QMessageBox::warning(this, tr("Validation Error"), tr("Please enter a proposal name."));
        return;
    }

    // Validate name characters (a-z, 0-9, - and _ only).
    QRegExp nameRx("[a-z0-9_-]+");
    if (!nameRx.exactMatch(name)) {
        QMessageBox::warning(this, tr("Validation Error"),
            tr("Name may only contain lowercase letters, digits, hyphens and underscores."));
        return;
    }

    if (url.isEmpty() || url.length() < 4) {
        QMessageBox::warning(this, tr("Validation Error"), tr("Please enter a valid URL."));
        return;
    }

    bool amountOk = false;
    double amount = amountStr.toDouble(&amountOk);
    if (!amountOk || amount <= 0.0) {
        QMessageBox::warning(this, tr("Validation Error"), tr("Please enter a valid payment amount."));
        return;
    }

    if (address.isEmpty()) {
        QMessageBox::warning(this, tr("Validation Error"), tr("Please enter a payment address."));
        return;
    }

    // Confirm collateral payment.
    QMessageBox::StandardButton reply = QMessageBox::question(
        this, tr("Confirm Collateral"),
        tr("Creating this proposal requires a collateral payment.\n\n"
           "This fee is non-refundable. Do you wish to proceed?"),
        QMessageBox::Yes | QMessageBox::Cancel);

    if (reply != QMessageBox::Yes) return;

    // Build proposal JSON and hex-encode it.
    nTime = QDateTime::currentSecsSinceEpoch();
    nRevision = 1;

    // start_epoch = now, end_epoch = now + (payments * superblock_cycle_seconds).
    // Superblock cycle is ~30 days. Use 30*24*60*60 = 2592000 per payment.
    int64_t nCycleSeconds = 2592000; // ~30 days
    int64_t nStartEpoch = nTime;
    int64_t nEndEpoch = nTime + (payments * nCycleSeconds);

    UniValue proposal(UniValue::VOBJ);
    proposal.pushKV("type", 1); // GOVERNANCE_OBJECT_PROPOSAL
    proposal.pushKV("name", name.toStdString());
    proposal.pushKV("start_epoch", nStartEpoch);
    proposal.pushKV("end_epoch", nEndEpoch);
    proposal.pushKV("payment_amount", amount);
    proposal.pushKV("payment_address", address.toStdString());
    proposal.pushKV("url", url.toStdString());

    std::string jsonStr = proposal.write();
    dataHex = QString::fromStdString(HexStr(std::vector<unsigned char>(jsonStr.begin(), jsonStr.end())));

    // Execute: gobject prepare 0 1 <time> <data-hex>
    std::string cmd = "gobject prepare 0 " + std::to_string(nRevision) + " " +
                      std::to_string(nTime) + " " + dataHex.toStdString();

    std::string result;
    bool ok = executeRpc(cmd, result);

    if (!ok) {
        ui->labelStatus->setText(tr("Prepare failed: %1").arg(QString::fromStdString(result)));
        return;
    }

    // Result is the collateral transaction hash.
    collateralHash = QString::fromStdString(result).trimmed();
    // Strip quotes if present (RPC may return JSON string).
    if (collateralHash.startsWith('"')) collateralHash = collateralHash.mid(1);
    if (collateralHash.endsWith('"')) collateralHash.chop(1);

    ui->labelStatus->setText(
        tr("Collateral transaction sent: %1\n\nWaiting for 6 confirmations...").arg(collateralHash));

    // Disable form fields and start polling for confirmations.
    setFormEnabled(false);
    confirmTimer->start(15000); // Check every 15 seconds.
}

void ProposalDialog::checkConfirmations()
{
    if (collateralHash.isEmpty()) return;

    // Query confirmation count via gettransaction.
    std::string result;
    std::string cmd = "gettransaction " + collateralHash.toStdString();
    if (!executeRpc(cmd, result)) {
        return; // Transaction not yet available, keep waiting.
    }

    UniValue txObj(UniValue::VOBJ);
    if (!txObj.read(result)) return;

    UniValue confVal = find_value(txObj, "confirmations");
    if (!confVal.isNum()) return;

    int nConf = confVal.get_int();
    if (nConf >= 6) {
        confirmTimer->stop();
        ui->buttonSubmit->setEnabled(true);
        ui->labelStatus->setText(
            tr("Collateral confirmed (%1 confirmations).\n\nClick 'Submit Proposal' to broadcast to the network.").arg(nConf));
    } else {
        ui->labelStatus->setText(
            tr("Collateral transaction: %1\n\nWaiting for confirmations: %2 / 6").arg(collateralHash).arg(nConf));
    }
}

void ProposalDialog::onSubmitClicked()
{
    if (collateralHash.isEmpty() || dataHex.isEmpty()) {
        QMessageBox::warning(this, tr("Submit Error"), tr("No prepared proposal found. Please prepare first."));
        return;
    }

    // Execute: gobject submit 0 <revision> <time> <data-hex> <collateral-txid>
    std::string cmd = "gobject submit 0 " + std::to_string(nRevision) + " " +
                      std::to_string(nTime) + " " + dataHex.toStdString() + " " +
                      collateralHash.toStdString();

    std::string result;
    bool ok = executeRpc(cmd, result);

    if (ok) {
        QString govHash = QString::fromStdString(result).trimmed();
        if (govHash.startsWith('"')) govHash = govHash.mid(1);
        if (govHash.endsWith('"')) govHash.chop(1);

        ui->labelStatus->setText(tr("Proposal submitted successfully!\n\nGovernance object hash: %1").arg(govHash));
        ui->buttonSubmit->setEnabled(false);

        QMessageBox::information(this, tr("Proposal Submitted"),
            tr("Your proposal has been submitted to the network.\n\n"
               "Governance hash: %1\n\n"
               "It may take a few minutes for the proposal to appear in the list. "
               "Masternode owners can now vote on it.").arg(govHash));
    } else {
        ui->labelStatus->setText(tr("Submit failed: %1").arg(QString::fromStdString(result)));
        QMessageBox::critical(this, tr("Submit Failed"), QString::fromStdString(result));
    }
}
