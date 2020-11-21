#ifndef ANONMSGPAGE_H
#define ANONMSGPAGE_H

#include "platformstyle.h"
#include "sync.h"
#include "util.h"

#include <QTimer>
#include <QWidget>

namespace Ui {
    class AnonmsgPage;
}

class ClientModel;
class WalletModel;

QT_BEGIN_NAMESPACE
class QModelIndex;
QT_END_NAMESPACE

class AnonmsgPage : public QWidget
{
    Q_OBJECT

public:
    explicit AnonmsgPage(const PlatformStyle *platformStyle, QWidget *parent = 0);
    ~AnonmsgPage();

    void setClientModel(ClientModel *clientModel);
    void setWalletModel(WalletModel *walletModel);

private:
    QTimer *timer;
    Ui::AnonmsgPage *ui;
    ClientModel *clientModel;
    WalletModel *walletModel;

private Q_SLOTS:
    void updateMessageBoard();
    void on_sendButton_clicked();
};
#endif // ANONMSGPAGE_H
