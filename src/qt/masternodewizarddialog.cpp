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
    return RPCConsole::RPCExecuteCommandLine(m_node, result, command, nullptr, walletModel);
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
        summary += tr("Server: %1:%2").arg(ui->lineEditIpAddress->text(), ui->lineEditPort->text()) + "\n\n";
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

bool MasternodeWizardDialog::registerMasternode()
{
    // Build the protx register_fund command
    QString ip = ui->lineEditIpAddress->text().trimmed();
    QString port = ui->lineEditPort->text().trimmed();
    QString ipPort = ip + ":" + port;

    // Generate a collateral address
    std::string result;
    if (!executeRpc("getnewaddress \"mn-collateral\"", result)) {
        ui->labelStatus->setText(tr("Failed to generate collateral address: %1").arg(QString::fromStdString(result)));
        return false;
    }
    QString collateralAddress = QString::fromStdString(result).trimmed();
    if (collateralAddress.startsWith('"') && collateralAddress.endsWith('"')) {
        collateralAddress = collateralAddress.mid(1, collateralAddress.length() - 2);
    }

    // protx register_fund "collateralAddress" "ipAndPort" "ownerAddress" "operatorPubKey" "votingAddress" operatorReward "payoutAddress"
    QString cmd = QString("protx register_fund \"%1\" \"%2\" \"%3\" \"%4\" \"%5\" 0 \"%6\"")
        .arg(collateralAddress, ipPort, ownerAddress, blsPublicKey, votingAddress, payoutAddress);

    if (!executeRpc(cmd.toStdString(), result)) {
        ui->labelStatus->setStyleSheet("color: #c0392b;");
        ui->labelStatus->setText(tr("Registration failed: %1").arg(QString::fromStdString(result)));
        return false;
    }

    QString txid = QString::fromStdString(result).trimmed();
    if (txid.startsWith('"') && txid.endsWith('"')) {
        txid = txid.mid(1, txid.length() - 2);
    }

    QMessageBox::information(this, tr("Masternode Registered"),
        tr("Masternode registration transaction submitted successfully!\n\n"
           "Transaction ID:\n%1\n\n"
           "IMPORTANT: Add the following line to the masternode daemon configuration file on your server and restart it:\n\n"
           "masternodeblsprivkey=%2\n\n"
           "The masternode will become active after the transaction receives sufficient confirmations.")
        .arg(txid, blsSecretKey));

    return true;
}
