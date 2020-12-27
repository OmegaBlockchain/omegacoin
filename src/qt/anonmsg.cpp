// Copyright (c) 2020 barrystyle
// Copyright (c) 2020 Kolby Moroz Liebl
#include "anonmsg.h"
#include "ui_anonmsg.h"

#include "../anonmsg.h"
#include "clientmodel.h"
#include "init.h"
#include "guiutil.h"
#include "sync.h"
#include "wallet/wallet.h"
#include "walletmodel.h"

#include <QTimer>
#include <QMessageBox>
#include <boost/exception/to_string.hpp>

AnonmsgPage::AnonmsgPage(const PlatformStyle *platformStyle, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::AnonmsgPage),
    clientModel(0),
    walletModel(0)
{
    ui->setupUi(this);
    timer = new QTimer(this);
    connect(timer, SIGNAL(timeout()), this, SLOT(updateMessageBoard()));
    timer->start(1000);
}

AnonmsgPage::~AnonmsgPage()
{
    delete ui;
}

void AnonmsgPage::setClientModel(ClientModel *model)
{
    this->clientModel = model;
    if(model) {}
}

void AnonmsgPage::setWalletModel(WalletModel *model)
{
    this->walletModel = model;
}

void AnonmsgPage::updateMessageBoard()
{
    std::list<std::string> listMsg;
    bool newPending = getAnonMessages(listMsg);
    ui->textEdit->clear();
    for (std::list<std::string>::iterator nextMessage=listMsg.begin(); nextMessage!=listMsg.end(); ++nextMessage) {
        ui->textEdit->append(QString::fromStdString(nextMessage));
    }
}

void AnonmsgPage::on_sendButton_clicked()
{
    QString sendTextQ = ui->textEdit_2->toPlainText();
    ui->textEdit_2->clear();
    std::string strMsg = sendTextQ.toStdString();

    if (strMsg.empty()) {
        return;
    }

    //! create and relay a message
    CAnonMsg testCase;
    testCase.setMessage(strMsg);

    //! relay message and store
    testCase.Relay();
    mapAnonMsg.insert(std::make_pair(testCase.GetHash(),testCase));
    return;
}
