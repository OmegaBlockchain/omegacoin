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
    ui->textEdit->clear();
    std::map<int64_t, std::string> messagesToSort;
    for (auto message=mapAnonMsg.begin(); message!=mapAnonMsg.end(); ++message) {
        messagesToSort.insert(std::make_pair(message->second.getTime(), message->second.getMessage()));
    }
    sortmap(messagesToSort);
    for (auto message=messagesToSort.begin(); message!=messagesToSort.end(); ++message) {
        std::string msgpayload = message->second;
        int64_t msgtime = message->first;
        //if (msgpayload.size() > 256) return false;
        std::string messageStr = msgpayload +" "+"("+boost::to_string(msgtime)+")";
        ui->textEdit->append(QString::fromStdString(messageStr));
    }
}

void AnonmsgPage::on_sendButton_clicked()
{
    QString sendTextQ = ui->textEdit_2->toPlainText();
    ui->textEdit_2->clear();
    std::string strMsg = sendTextQ.toStdString();

    if (strMsg.empty() || (strMsg.size() > 141)) {
        return;
    }

    //! create and relay a message
    CAnonMsg testCase;
    testCase.setMessage(strMsg);

    //! relay message and store
    testCase.Relay(*g_connman);
    mapAnonMsg.insert(std::make_pair(testCase.GetHash(),testCase));
    return;
}
