// Copyright (c) 2024 The Omega developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/masternodewizarddialog.h>
#include <qt/forms/ui_masternodewizarddialog.h>

#include <qt/guiutil.h>
#include <qt/rpcconsole.h>
#include <qt/walletmodel.h>
#include <chainparams.h>
#include <evo/dmn_types.h>
#include <util/system.h>

#include <univalue.h>

#include <cmath>
#include <map>
#include <set>

#include <QIntValidator>
#include <QMessageBox>
#include <QRegularExpression>

MasternodeWizardDialog::MasternodeWizardDialog(interfaces::Node& node, WalletModel* model, QWidget* parent) :
    QDialog(parent),
    ui(new Ui::MasternodeWizardDialog),
    m_node(node),
    walletModel(model)
{
    ui->setupUi(this);

    // Set default port
    ui->lineEditPort->setText(QString::number(Params().GetDefaultPort()));

    // IP address validation
    ui->lineEditIpAddress->setValidator(new QRegularExpressionValidator(
        QRegularExpression("^\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}\\.\\d{1,3}$"), this));

    connect(ui->btnNext,     &QPushButton::clicked,  this, &MasternodeWizardDialog::onNextClicked);
    connect(ui->btnBack,     &QPushButton::clicked,  this, &MasternodeWizardDialog::onBackClicked);
    connect(ui->btnCancel,   &QPushButton::clicked,  this, &MasternodeWizardDialog::reject);
    connect(ui->btnGenerate, &QPushButton::clicked,  this, &MasternodeWizardDialog::onGenerateKeysClicked);
    connect(ui->radioRegular, &QRadioButton::toggled, this, &MasternodeWizardDialog::onMnTypeChanged);
    connect(ui->radioHP,      &QRadioButton::toggled, this, &MasternodeWizardDialog::onMnTypeChanged);

    // Pre-fill fixed platform ports (enforced by consensus on mainnet)
    ui->lineEditPlatformP2PPort->setText(QString::number(Params().GetDefaultPlatformP2PPort()));
    ui->lineEditPlatformHTTPPort->setText(QString::number(Params().GetDefaultPlatformHTTPPort()));

    updateStepDisplay();
    GUIUtil::updateFonts();
}

MasternodeWizardDialog::~MasternodeWizardDialog()
{
    delete ui;
}

QString MasternodeWizardDialog::translateRpcError(const QString& raw) const
{
    // Map raw RPC / consensus error strings to user-friendly messages.
    // Each entry: substring to match, error code, explanation, suggested fix.
    struct ErrorMapping {
        const char* pattern;
        const char* code;
        const char* message;
        const char* solution;
    };

    static const ErrorMapping mappings[] = {
        // -- Duplicate resource errors --
        {"bad-protx-dup-addr",    "MNW-401",
         "This IP address is already registered to another masternode.",
         "Use a different IP address, or if you own the existing masternode, "
         "update it instead of registering a new one."},

        {"bad-protx-dup-key",     "MNW-402",
         "The owner key or operator key is already in use by another masternode.",
         "Go back and generate a new set of keys."},

        {"bad-protx-dup-platformnodeid", "MNW-403",
         "The Platform Node ID is already registered to another masternode.",
         "Assign a unique Platform Node ID."},

        // -- Collateral errors --
        {"bad-protx-collateral-reuse", "MNW-301",
         "The collateral address must not be the same as the owner or voting address.",
         "Go back and generate fresh addresses."},

        {"bad-protx-collateral-pkh", "MNW-302",
         "The collateral must be held in a standard (P2PKH) address.",
         "Send the collateral to a regular wallet address, not a multisig or script address."},

        {"bad-protx-collateral",  "MNW-303",
         "The collateral transaction is missing, already spent, or the wrong amount.",
         "Ensure your wallet has an unspent output of exactly the required collateral amount."},

        // -- Address / key errors --
        {"bad-protx-payee-reuse", "MNW-304",
         "The payout address must not be the same as the owner or voting address.",
         "Go back and generate fresh addresses."},

        {"bad-protx-payee",       "MNW-305",
         "The payout address is not a valid standard address.",
         "Ensure the payout address is a regular (P2PKH or P2SH) address."},

        {"bad-protx-key-null",    "MNW-306",
         "One or more required keys are missing.",
         "Go back and regenerate keys and addresses."},

        {"bad-protx-operator-pubkey", "MNW-307",
         "The BLS operator key uses an incompatible scheme.",
         "Regenerate the BLS key pair."},

        // -- Network address errors --
        {"bad-protx-ipaddr-port", "MNW-404",
         "The port number is not allowed for this network.",
         "Use the default port shown in the wizard."},

        {"bad-protx-ipaddr",      "MNW-405",
         "The IP address is not a valid, publicly routable IPv4 address.",
         "Enter the public IP of your server (not a LAN or localhost address)."},

        // -- Signature / authorisation errors --
        {"bad-protx-sig",         "MNW-501",
         "The registration transaction could not be signed.",
         "Ensure the wallet is unlocked and contains the private key for the owner address."},

        {"failed to sign special tx", "MNW-502",
         "The wallet could not sign the registration transaction.",
         "Unlock the wallet (Settings > Unlock Wallet), then try again."},

        // -- Funding errors --
        {"Insufficient funds",    "MNW-310",
         "Not enough funds to complete the registration.",
         "Your wallet needs the collateral amount plus a small fee. "
         "Check your balance and wait for any pending transactions to confirm."},

        {"No funds at specified address", "MNW-311",
         "The selected funding address has no confirmed balance.",
         "Wait for pending transactions to confirm, or send funds to this wallet."},

        {"Fee estimation failed",  "MNW-312",
         "The network fee could not be estimated.",
         "Wait for the blockchain to sync fully, then try again."},

        // -- Wallet state errors --
        {"wallet is locked",       "MNW-510",
         "The wallet is locked.",
         "Unlock the wallet (Settings > Unlock Wallet), then try again."},

        {"Wallet model no longer available", "MNW-511",
         "The wallet was closed while the wizard was open.",
         "Close the wizard and reopen it."},

        {"Private key for owner address", "MNW-512",
         "The wallet does not hold the private key for the owner address.",
         "The owner address must belong to this wallet."},

        // -- Payload / protocol errors --
        {"bad-protx-payload",      "MNW-601",
         "The registration data could not be processed by the network.",
         "This may indicate a version mismatch. Ensure your wallet is up to date."},

        {"bad-protx-version",      "MNW-602",
         "Unsupported masternode registration version.",
         "Update your wallet to the latest release."},

        {"bad-protx-type",         "MNW-603",
         "The masternode type is not supported on this network.",
         "Ensure you are on the correct network and your wallet is up to date."},

        {"bad-protx-hpmn-not-active", "MNW-604",
         "High Performance masternodes are not yet active on this network.",
         "HP masternodes activate at block 3,200,000. Check the current block height and try again later."},
    };

    for (const auto& m : mappings) {
        if (raw.contains(QLatin1String(m.pattern), Qt::CaseInsensitive)) {
            return tr("[%1] %2\n\nSuggestion: %3")
                .arg(QLatin1String(m.code),
                     tr(m.message),
                     tr(m.solution));
        }
    }

    // No known pattern — return the raw text with a generic code
    return tr("[MNW-900] Unexpected error: %1\n\n"
              "Suggestion: Check that the blockchain is fully synced and the wallet is unlocked. "
              "If the problem persists, copy this error and ask for help.")
        .arg(raw);
}

bool MasternodeWizardDialog::executeRpc(const std::string& command, std::string& result)
{
    if (!walletModel) {
        result = "Wallet model no longer available.";
        return false;
    }
    try {
        return RPCConsole::RPCExecuteCommandLine(m_node, result, command, nullptr, walletModel.data());
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

void MasternodeWizardDialog::onMnTypeChanged()
{
    m_mnType = ui->radioHP->isChecked() ? MnType::HighPerformance : MnType::Regular;
    ui->groupBoxPlatform->setVisible(m_mnType == MnType::HighPerformance);
}

void MasternodeWizardDialog::updateStepDisplay()
{
    ui->stackedWidget->setCurrentIndex(currentStep);
    ui->labelStepIndicator->setText(tr("Step %1 of 4").arg(currentStep + 1));
    ui->btnBack->setVisible(currentStep > 0);
    ui->labelStatus->clear();

    if (currentStep == 3) {
        ui->btnNext->setText(tr("Register"));

        const auto& mnTypeInfo = GetMnType(m_mnType);
        QString summary;
        summary += tr("Type: %1").arg(QLatin1String(mnTypeInfo.description.data())) + "\n\n";
        summary += tr("Collateral: %1 OMEGA").arg(QString::number(mnTypeInfo.collat_amount / COIN)) + "\n\n";
        summary += tr("Governance votes: %1").arg(mnTypeInfo.voting_weight) + "\n\n";
        summary += tr("Server: %1:%2").arg(cachedIpAddress, cachedPort) + "\n\n";
        summary += tr("Owner Address: %1").arg(ownerAddress) + "\n\n";
        summary += tr("Payout Address: %1").arg(payoutAddress) + "\n\n";
        summary += tr("Voting Address: %1").arg(votingAddress) + "\n\n";
        summary += tr("BLS Public Key: %1").arg(blsPublicKey) + "\n\n";
        summary += tr("BLS Secret Key: %1").arg(blsSecretKey) + "\n\n";
        summary += tr("Operator Reward: 0%");
        if (m_mnType == MnType::HighPerformance) {
            summary += "\n\n" + tr("Platform Node ID: %1").arg(ui->lineEditPlatformNodeID->text().trimmed());
            summary += "\n" + tr("Platform P2P Port: %1").arg(ui->lineEditPlatformP2PPort->text());
            summary += "\n" + tr("Platform HTTP Port: %1").arg(ui->lineEditPlatformHTTPPort->text());
        }
        ui->textEditSummary->setPlainText(summary);
    } else {
        ui->btnNext->setText(tr("Next"));
    }
}

void MasternodeWizardDialog::onNextClicked()
{
    ui->labelStatus->clear();

    switch (currentStep) {
    case 0:
        // Welcome page - just advance
        currentStep++;
        break;
    case 1: {
        // Validate IP address
        QString ip = ui->lineEditIpAddress->text().trimmed();
        if (ip.isEmpty()) {
            ui->labelStatus->setText(tr("Please enter an IP address."));
            return;
        }
        QStringList octets = ip.split('.');
        if (octets.size() != 4) {
            ui->labelStatus->setText(tr("Invalid IP address format."));
            return;
        }
        for (const QString& octet : octets) {
            bool ok;
            int val = octet.toInt(&ok);
            if (!ok || val < 0 || val > 255) {
                ui->labelStatus->setText(tr("Invalid IP address."));
                return;
            }
        }
        if (m_mnType == MnType::HighPerformance) {
            QString nodeID = ui->lineEditPlatformNodeID->text().trimmed();
            QRegularExpression hexRe("^[0-9a-fA-F]{40}$");
            if (!hexRe.match(nodeID).hasMatch()) {
                ui->labelStatus->setText(tr("Platform Node ID must be exactly 40 hex characters."));
                return;
            }
        }
        cachedIpAddress = ip;
        cachedPort = ui->lineEditPort->text().trimmed();
        currentStep++;
        break;
    }
    case 2:
        // Validate keys are generated
        if (blsPublicKey.isEmpty() || ownerAddress.isEmpty()) {
            ui->labelStatus->setText(tr("Please generate keys and addresses first."));
            return;
        }
        currentStep++;
        break;
    case 3:
        // Register
        if (registerMasternode()) {
            QDialog::accept();
        }
        return;
    }

    updateStepDisplay();
}

void MasternodeWizardDialog::onBackClicked()
{
    if (currentStep > 0) {
        currentStep--;
        updateStepDisplay();
    }
}

bool MasternodeWizardDialog::generateAddresses()
{
    std::string result;

    // Generate owner address
    if (!executeRpc("getnewaddress \"mn-owner\"", result)) {
        ui->labelStatus->setText(translateRpcError(QString::fromStdString(result)));
        return false;
    }
    ownerAddress = QString::fromStdString(result).trimmed();
    // Remove quotes if present
    if (ownerAddress.startsWith('"') && ownerAddress.endsWith('"')) {
        ownerAddress = ownerAddress.mid(1, ownerAddress.length() - 2);
    }

    // Generate payout address
    if (!executeRpc("getnewaddress \"mn-payout\"", result)) {
        ui->labelStatus->setText(translateRpcError(QString::fromStdString(result)));
        return false;
    }
    payoutAddress = QString::fromStdString(result).trimmed();
    if (payoutAddress.startsWith('"') && payoutAddress.endsWith('"')) {
        payoutAddress = payoutAddress.mid(1, payoutAddress.length() - 2);
    }

    // Voting address = owner address by default
    votingAddress = ownerAddress;

    return true;
}

bool MasternodeWizardDialog::generateBLSKeys()
{
    std::string result;
    if (!executeRpc("bls generate", result)) {
        ui->labelStatus->setText(translateRpcError(QString::fromStdString(result)));
        return false;
    }

    // Parse JSON result
    UniValue json(UniValue::VOBJ);
    if (!json.read(result)) {
        ui->labelStatus->setText(tr("[MNW-210] Could not read BLS key data.\n\n"
                                    "Suggestion: Try generating keys again. If this persists, restart the wallet."));
        return false;
    }

    if (!json.exists("secret") || !json.exists("public")) {
        ui->labelStatus->setText(tr("[MNW-211] BLS key generation returned incomplete data.\n\n"
                                    "Suggestion: Try generating keys again. If this persists, restart the wallet."));
        return false;
    }

    blsSecretKey = QString::fromStdString(json["secret"].get_str());
    blsPublicKey = QString::fromStdString(json["public"].get_str());

    return true;
}

void MasternodeWizardDialog::onGenerateKeysClicked()
{
    ui->labelStatus->clear();
    ui->btnGenerate->setEnabled(false);

    bool success = generateAddresses() && generateBLSKeys();

    if (success) {
        ui->lineEditOwnerAddress->setText(ownerAddress);
        ui->lineEditPayoutAddress->setText(payoutAddress);
        ui->lineEditBlsPublicKey->setText(blsPublicKey);
        ui->lineEditBlsSecretKey->setText(blsSecretKey);
        ui->labelStatus->setStyleSheet("color: #27ae60;");
        ui->labelStatus->setText(tr("Keys and addresses generated successfully."));
    } else {
        ui->btnGenerate->setEnabled(true);
    }
}

bool MasternodeWizardDialog::findExistingCollateral(QString& txid, int& vout)
{
    std::string result;

    // Get all confirmed UTXOs (no JSON query_options — the RPC command
    // parser treats commas inside {} as argument separators).
    if (!executeRpc("listunspent 1 9999999", result))
        return false;

    UniValue utxos(UniValue::VARR);
    if (!utxos.read(result) || utxos.empty())
        return false;

    // Filter to UTXOs that are exactly the collateral amount for the selected type
    CAmount collatAmount = GetMnType(m_mnType).collat_amount;
    double collatCoins = (double)collatAmount / COIN;

    // Collect all registered masternode collateral outpoints
    std::set<std::pair<std::string, int>> usedCollaterals;
    if (executeRpc("protx list registered true", result)) {
        UniValue mnList(UniValue::VARR);
        if (mnList.read(result)) {
            for (size_t i = 0; i < mnList.size(); i++) {
                const UniValue& mn = mnList[i];
                if (mn.exists("collateralHash") && mn.exists("collateralIndex")) {
                    usedCollaterals.insert({
                        mn["collateralHash"].get_str(),
                        mn["collateralIndex"].get_int()
                    });
                }
            }
        }
    }

    // Return the first UTXO of exactly the collateral amount
    // that is not already used as masternode collateral
    for (size_t i = 0; i < utxos.size(); i++) {
        const UniValue& utxo = utxos[i];
        if (!utxo.exists("amount") || !utxo.exists("txid") || !utxo.exists("vout"))
            continue;

        double amount = utxo["amount"].get_real();
        if (std::abs(amount - collatCoins) > 0.00001)
            continue;

        std::string hash = utxo["txid"].get_str();
        int n = utxo["vout"].get_int();

        if (usedCollaterals.count({hash, n}) == 0) {
            txid = QString::fromStdString(hash);
            vout = n;
            return true;
        }
    }

    return false;
}

bool MasternodeWizardDialog::findCollateralForIP(const QString& ipPort, QString& txid, int& vout)
{
    // Check if the target IP:port is already registered to a MN whose
    // collateral UTXO is in our wallet.  Returning that outpoint lets
    // protx register replace the old MN instead of failing with dup-addr.
    std::string result;

    if (!executeRpc("protx list registered true", result))
        return false;

    UniValue mnList(UniValue::VARR);
    if (!mnList.read(result))
        return false;

    std::string targetAddr = ipPort.toStdString();

    for (size_t i = 0; i < mnList.size(); i++) {
        const UniValue& mn = mnList[i];
        if (!mn.exists("state"))
            continue;
        const UniValue& state = mn["state"];
        if (!state.exists("service"))
            continue;
        if (state["service"].get_str() != targetAddr)
            continue;

        // This MN uses our IP — check if the collateral is in our wallet
        if (!mn.exists("collateralHash") || !mn.exists("collateralIndex"))
            continue;

        std::string hash = mn["collateralHash"].get_str();
        int idx = mn["collateralIndex"].get_int();

        // Verify we own this UTXO via gettxout (it must still be unspent)
        std::string txoutResult;
        std::string txoutCmd = "gettxout \"" + hash + "\" " + std::to_string(idx);
        if (!executeRpc(txoutCmd, txoutResult))
            continue;

        UniValue txout(UniValue::VOBJ);
        if (!txout.read(txoutResult) || txout.isNull())
            continue;

        // Check the address is in our wallet
        std::string addrCheckResult;
        if (txout.exists("scriptPubKey")) {
            const UniValue& spk = txout["scriptPubKey"];
            if (spk.exists("address")) {
                std::string addr = spk["address"].get_str();
                std::string dumpResult;
                if (executeRpc("dumpprivkey \"" + addr + "\"", dumpResult)) {
                    txid = QString::fromStdString(hash);
                    vout = idx;
                    return true;
                }
            }
        }
    }

    return false;
}

bool MasternodeWizardDialog::findFundingAddress(QString& address, double minCoins,
                                                 const QString& excludeTxid, int excludeVout)
{
    std::string result;

    if (!executeRpc("listunspent 1 9999999", result))
        return false;

    UniValue utxos(UniValue::VARR);
    if (!utxos.read(result) || utxos.empty())
        return false;

    std::string excludeHash = excludeTxid.toStdString();

    // Sum available balance per address, skipping the excluded UTXO
    std::map<std::string, double> addressBalances;
    for (size_t i = 0; i < utxos.size(); i++) {
        const UniValue& utxo = utxos[i];
        if (!utxo.exists("address") || !utxo.exists("amount"))
            continue;

        // Skip the collateral UTXO so it is not counted as fee funds
        if (!excludeHash.empty() && excludeVout >= 0 &&
            utxo.exists("txid") && utxo.exists("vout") &&
            utxo["txid"].get_str() == excludeHash &&
            utxo["vout"].get_int() == excludeVout) {
            continue;
        }

        addressBalances[utxo["address"].get_str()] += utxo["amount"].get_real();
    }

    // Pick the address with the highest balance that meets the minimum
    std::string bestAddr;
    double bestBalance = 0;
    for (const auto& entry : addressBalances) {
        if (entry.second >= minCoins && entry.second > bestBalance) {
            bestAddr = entry.first;
            bestBalance = entry.second;
        }
    }

    if (bestAddr.empty())
        return false;

    address = QString::fromStdString(bestAddr);
    return true;
}

bool MasternodeWizardDialog::registerMasternode()
{
    QString ipPort = cachedIpAddress + ":" + cachedPort;
    std::string result;
    bool registered = false;

    // Strategy 0: The IP:port is already registered to a MN whose collateral
    // is in our wallet.  Reuse that outpoint so protx register replaces the
    // old MN instead of failing with bad-protx-dup-addr.
    QString collateralHash;
    int collateralIndex = -1;
    findCollateralForIP(ipPort, collateralHash, collateralIndex);

    const CAmount collatAmount = GetMnType(m_mnType).collat_amount;
    const bool isHP = (m_mnType == MnType::HighPerformance);
    const QString platformNodeID  = isHP ? ui->lineEditPlatformNodeID->text().trimmed()  : QString();
    const QString platformP2PPort = isHP ? ui->lineEditPlatformP2PPort->text().trimmed()  : QString();
    const QString platformHTTPPort= isHP ? ui->lineEditPlatformHTTPPort->text().trimmed() : QString();

    // Strategy 1: Use an existing unused collateral UTXO of the correct amount.
    if (collateralIndex < 0) {
        findExistingCollateral(collateralHash, collateralIndex);
    }

    if (collateralIndex >= 0) {
        QString feeSourceAddress;
        QString cmd;
        findFundingAddress(feeSourceAddress, 0.001, collateralHash, collateralIndex);

        if (isHP) {
            // protx register_hpmn "hash" index "ip" "owner" "blsPub" "voting" reward "payout" "nodeID" p2pPort httpPort ["feeSource"]
            if (!feeSourceAddress.isEmpty()) {
                cmd = QString("protx register_hpmn \"%1\" %2 \"%3\" \"%4\" \"%5\" \"%6\" 0 \"%7\" \"%8\" %9 %10 \"%11\"")
                    .arg(collateralHash).arg(collateralIndex)
                    .arg(ipPort, ownerAddress, blsPublicKey, votingAddress, payoutAddress)
                    .arg(platformNodeID, platformP2PPort, platformHTTPPort, feeSourceAddress);
            } else {
                cmd = QString("protx register_hpmn \"%1\" %2 \"%3\" \"%4\" \"%5\" \"%6\" 0 \"%7\" \"%8\" %9 %10")
                    .arg(collateralHash).arg(collateralIndex)
                    .arg(ipPort, ownerAddress, blsPublicKey, votingAddress, payoutAddress)
                    .arg(platformNodeID, platformP2PPort, platformHTTPPort);
            }
        } else {
            if (!feeSourceAddress.isEmpty()) {
                // protx register "hash" index "ip" "owner" "blsPub" "voting" reward "payout" "feeSource"
                cmd = QString("protx register \"%1\" %2 \"%3\" \"%4\" \"%5\" \"%6\" 0 \"%7\" \"%8\"")
                    .arg(collateralHash).arg(collateralIndex)
                    .arg(ipPort, ownerAddress, blsPublicKey, votingAddress, payoutAddress, feeSourceAddress);
            } else {
                cmd = QString("protx register \"%1\" %2 \"%3\" \"%4\" \"%5\" \"%6\" 0 \"%7\"")
                    .arg(collateralHash).arg(collateralIndex)
                    .arg(ipPort, ownerAddress, blsPublicKey, votingAddress, payoutAddress);
            }
        }

        registered = executeRpc(cmd.toStdString(), result);
        if (!registered) {
            ui->labelStatus->setStyleSheet("color: #c0392b;");
            if (feeSourceAddress.isEmpty()) {
                ui->labelStatus->setText(tr("[MNW-311] Found %1 OMEGA collateral but no additional funds to pay "
                                            "the transaction fee.\n\n"
                                            "Suggestion: Send a small amount (at least 0.001 OMEGA) to any "
                                            "address in this wallet and wait for it to confirm.")
                    .arg(QString::number(collatAmount / COIN)));
            } else {
                ui->labelStatus->setText(translateRpcError(QString::fromStdString(result)));
            }
            return false;
        }
    }

    // Strategy 2: No existing collateral found — fund a new one.
    if (!registered) {
        if (!executeRpc("getnewaddress \"mn-collateral\"", result)) {
            ui->labelStatus->setText(translateRpcError(QString::fromStdString(result)));
            return false;
        }
        QString collateralAddress = QString::fromStdString(result).trimmed();
        if (collateralAddress.startsWith('"') && collateralAddress.endsWith('"'))
            collateralAddress = collateralAddress.mid(1, collateralAddress.length() - 2);

        QString cmd;
        if (isHP) {
            // protx register_fund_hpmn "collateral" "ip" "owner" "blsPub" "voting" reward "payout" "nodeID" p2pPort httpPort
            cmd = QString("protx register_fund_hpmn \"%1\" \"%2\" \"%3\" \"%4\" \"%5\" 0 \"%6\" \"%7\" %8 %9")
                .arg(collateralAddress, ipPort, ownerAddress, blsPublicKey, votingAddress, payoutAddress)
                .arg(platformNodeID, platformP2PPort, platformHTTPPort);
        } else {
            // protx register_fund "collateral" "ip" "owner" "blsPub" "voting" reward "payout"
            cmd = QString("protx register_fund \"%1\" \"%2\" \"%3\" \"%4\" \"%5\" 0 \"%6\"")
                .arg(collateralAddress, ipPort, ownerAddress, blsPublicKey, votingAddress, payoutAddress);
        }

        if (!executeRpc(cmd.toStdString(), result)) {
            ui->labelStatus->setStyleSheet("color: #c0392b;");
            ui->labelStatus->setText(translateRpcError(QString::fromStdString(result)));
            return false;
        }
    }

    QString txid = QString::fromStdString(result).trimmed();
    if (txid.startsWith('"') && txid.endsWith('"'))
        txid = txid.mid(1, txid.length() - 2);

    QMessageBox::information(this, tr("Masternode Registered"),
        tr("Masternode registration transaction submitted successfully!\n\n"
           "Transaction ID:\n%1\n\n"
           "IMPORTANT: Add the following line to the masternode daemon configuration file on your server and restart it:\n\n"
           "masternodeblsprivkey=%2\n\n"
           "The masternode will become active after the transaction receives sufficient confirmations.")
        .arg(txid, blsSecretKey));

    return true;
}
