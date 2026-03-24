// Copyright (c) 2026 The Omega Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PROPOSALDIALOG_H
#define BITCOIN_QT_PROPOSALDIALOG_H

#include <QDialog>
#include <QTimer>

class WalletModel;

namespace Ui {
class ProposalDialog;
}

class ProposalDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ProposalDialog(WalletModel* walletModel, QWidget* parent = nullptr);
    ~ProposalDialog();

private Q_SLOTS:
    void onPrepareClicked();
    void onSubmitClicked();
    void checkConfirmations();

private:
    Ui::ProposalDialog* ui;
    WalletModel* walletModel;
    QTimer* confirmTimer;

    // Saved proposal parameters for submit stage.
    int nRevision{1};
    int64_t nTime{0};
    QString dataHex;
    QString collateralHash;

    bool executeRpc(const std::string& command, std::string& result);
    void setFormEnabled(bool enabled);
};

#endif // BITCOIN_QT_PROPOSALDIALOG_H
