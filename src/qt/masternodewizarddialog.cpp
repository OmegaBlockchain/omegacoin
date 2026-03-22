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

    connect(ui->btnNext, &QPushButton::clicked, this, &MasternodeWizardDialog::onNextClicked);
    connect(ui->btnBack, &QPushButton::clicked, this, &MasternodeWizardDialog::onBackClicked);
    connect(ui->btnCancel, &QPushButton::clicked, this, &MasternodeWizardDialog::reject);
    connect(ui->btnGenerate, &QPushButton::clicked, this, &MasternodeWizardDialog::onGenerateKeysClicked);

    updateStepDisplay();
    GUIUtil::updateFonts();
}

MasternodeWizardDialog::~MasternodeWizardDialog()
{
    delete ui;
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

void MasternodeWizardDialog::updateStepDisplay()
{
    ui->stackedWidget->setCurrentIndex(currentStep);
    ui->labelStepIndicator->setText(tr("Step %1 of 4").arg(currentStep + 1));
    ui->btnBack->setVisible(currentStep > 0);
    ui->labelStatus->clear();

    if (currentStep == 3) {
        ui->btnNext->setText(tr("Register"));

        QString summary;
        summary += tr("Server: %1:%2").arg(cachedIpAddress, cachedPort) + "\n\n";
        summary += tr("Owner Address: %1").arg(ownerAddress) + "\n\n";
        summary += tr("Payout Address: %1").arg(payoutAddress) + "\n\n";
        summary += tr("Voting Address: %1").arg(votingAddress) + "\n\n";
        summary += tr("BLS Public Key: %1").arg(blsPublicKey) + "\n\n";
        summary += tr("BLS Secret Key: %1").arg(blsSecretKey) + "\n\n";
        summary += tr("Operator Reward: 0%") + "\n\n";
        summary += tr("Collateral: %1").arg(QString::number(dmn_types::Regular.collat_amount / COIN));
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
        ui->labelStatus->setText(tr("Failed to generate owner address: %1").arg(QString::fromStdString(result)));
        return false;
    }
    ownerAddress = QString::fromStdString(result).trimmed();
    // Remove quotes if present
    if (ownerAddress.startsWith('"') && ownerAddress.endsWith('"')) {
        ownerAddress = ownerAddress.mid(1, ownerAddress.length() - 2);
    }

    // Generate payout address
    if (!executeRpc("getnewaddress \"mn-payout\"", result)) {
        ui->labelStatus->setText(tr("Failed to generate payout address: %1").arg(QString::fromStdString(result)));
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
        ui->labelStatus->setText(tr("Failed to generate BLS keys: %1").arg(QString::fromStdString(result)));
        return false;
    }

    // Parse JSON result
    UniValue json(UniValue::VOBJ);
    if (!json.read(result)) {
        ui->labelStatus->setText(tr("Failed to parse BLS key result."));
        return false;
    }

    if (!json.exists("secret") || !json.exists("public")) {
        ui->labelStatus->setText(tr("BLS key result missing fields."));
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

    // Filter to UTXOs that are exactly the collateral amount
    CAmount collatAmount = dmn_types::Regular.collat_amount;
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

    // Strategy 1: Use an existing (unused) 1000 OMEGA UTXO as collateral.
    if (collateralIndex < 0) {
        findExistingCollateral(collateralHash, collateralIndex);
    }

    if (collateralIndex >= 0) {
        QString feeSourceAddress;
        QString cmd;
        if (findFundingAddress(feeSourceAddress, 0.001, collateralHash, collateralIndex)) {
            // protx register "hash" index "ip" "owner" "blsPub" "voting" reward "payout" "feeSource"
            cmd = QString("protx register \"%1\" %2 \"%3\" \"%4\" \"%5\" \"%6\" 0 \"%7\" \"%8\"")
                .arg(collateralHash)
                .arg(collateralIndex)
                .arg(ipPort, ownerAddress, blsPublicKey, votingAddress, payoutAddress, feeSourceAddress);
        } else {
            // No separate fee source — let the wallet pick one automatically
            cmd = QString("protx register \"%1\" %2 \"%3\" \"%4\" \"%5\" \"%6\" 0 \"%7\"")
                .arg(collateralHash)
                .arg(collateralIndex)
                .arg(ipPort, ownerAddress, blsPublicKey, votingAddress, payoutAddress);
        }

        registered = executeRpc(cmd.toStdString(), result);
        if (!registered) {
            ui->labelStatus->setStyleSheet("color: #c0392b;");
            if (feeSourceAddress.isEmpty()) {
                ui->labelStatus->setText(tr("Found %1 OMEGA collateral but no additional funds to pay the transaction fee. "
                                            "Please send a small amount (at least 0.001 OMEGA) to any address in this wallet.")
                    .arg(QString::number(dmn_types::Regular.collat_amount / COIN)));
            } else {
                ui->labelStatus->setText(tr("Registration failed: %1").arg(QString::fromStdString(result)));
            }
            return false;
        }
    }

    // Strategy 2: No existing collateral found — fund a new one.
    // The wallet's coin selection gathers inputs from all addresses, so
    // funds do NOT need to sit in a single address.
    if (!registered) {
        // Generate a fresh collateral address
        if (!executeRpc("getnewaddress \"mn-collateral\"", result)) {
            ui->labelStatus->setText(tr("Failed to generate collateral address: %1").arg(QString::fromStdString(result)));
            return false;
        }
        QString collateralAddress = QString::fromStdString(result).trimmed();
        if (collateralAddress.startsWith('"') && collateralAddress.endsWith('"'))
            collateralAddress = collateralAddress.mid(1, collateralAddress.length() - 2);

        // protx register_fund "collateral" "ip" "owner" "blsPub" "voting" reward "payout"
        // Omit feeSourceAddress — the RPC defaults to using the payout address
        // for change, and coin selection picks inputs from the whole wallet.
        QString cmd = QString("protx register_fund \"%1\" \"%2\" \"%3\" \"%4\" \"%5\" 0 \"%6\"")
            .arg(collateralAddress, ipPort, ownerAddress, blsPublicKey, votingAddress, payoutAddress);

        if (!executeRpc(cmd.toStdString(), result)) {
            ui->labelStatus->setStyleSheet("color: #c0392b;");
            ui->labelStatus->setText(tr("Registration failed: %1").arg(QString::fromStdString(result)));
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
