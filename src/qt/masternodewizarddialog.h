// Copyright (c) 2024 The Omega developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_MASTERNODEWIZARDDIALOG_H
#define BITCOIN_QT_MASTERNODEWIZARDDIALOG_H

#include <QDialog>

namespace interfaces {
class Node;
}

class WalletModel;

namespace Ui {
class MasternodeWizardDialog;
}

class MasternodeWizardDialog : public QDialog
{
    Q_OBJECT

public:
    explicit MasternodeWizardDialog(interfaces::Node& node, WalletModel* walletModel, QWidget* parent = nullptr);
    ~MasternodeWizardDialog();

private Q_SLOTS:
    void onNextClicked();
    void onBackClicked();
    void onGenerateKeysClicked();

private:
    Ui::MasternodeWizardDialog* ui;
    interfaces::Node& m_node;
    WalletModel* walletModel;
    int currentStep{0};

    QString ownerAddress;
    QString payoutAddress;
    QString votingAddress;
    QString blsPublicKey;
    QString blsSecretKey;

    void updateStepDisplay();
    bool generateAddresses();
    bool generateBLSKeys();
    bool registerMasternode();
    bool executeRpc(const std::string& command, std::string& result);
};

#endif // BITCOIN_QT_MASTERNODEWIZARDDIALOG_H
