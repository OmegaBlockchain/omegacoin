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
    LogPrintf("%s called\n", __func__);
    std::string newMessage;
    bool newPending = getNextAnonMsg(newMessage);
    if (!newPending) return;
    ui->textEdit->append(QString::fromStdString(newMessage));
}

void AnonmsgPage::on_sendButton_clicked()
{
}
