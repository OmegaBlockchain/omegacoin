// Copyright (c) 2021-2022 The Dash Core developers
// Copyright (c) 2026 The Omega Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/forms/ui_governancelist.h>
#include <qt/governancelist.h>

#include <chainparams.h>
#include <clientversion.h>
#include <coins.h>
#include <evo/deterministicmns.h>
#include <governance/vote.h>
#include <netbase.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/proposaldialog.h>
#include <qt/rpcconsole.h>
#include <qt/walletmodel.h>

#include <univalue.h>

#include <QAbstractItemView>
#include <QApplication>
#include <QDesktopServices>
#include <QMessageBox>
#include <QTableWidgetItem>
#include <QUrl>
#include <QtGui/QClipboard>

///
/// Proposal wrapper
///

Proposal::Proposal(const CGovernanceObject& _govObj, QObject* parent) :
    QObject(parent),
    govObj(_govObj)
{
    UniValue prop_data;
    if (prop_data.read(govObj.GetDataAsPlainString())) {
        if (UniValue titleValue = find_value(prop_data, "name"); titleValue.isStr()) {
            m_title = QString::fromStdString(titleValue.get_str());
        }

        if (UniValue paymentStartValue = find_value(prop_data, "start_epoch"); paymentStartValue.isNum()) {
            m_startDate = QDateTime::fromSecsSinceEpoch(paymentStartValue.get_int64());
        }

        if (UniValue paymentEndValue = find_value(prop_data, "end_epoch"); paymentEndValue.isNum()) {
            m_endDate = QDateTime::fromSecsSinceEpoch(paymentEndValue.get_int64());
        }

        if (UniValue amountValue = find_value(prop_data, "payment_amount"); amountValue.isNum()) {
            m_paymentAmount = amountValue.get_real();
        }

        if (UniValue urlValue = find_value(prop_data, "url"); urlValue.isStr()) {
            m_url = QString::fromStdString(urlValue.get_str());
        }
    }
}

QString Proposal::title() const { return m_title; }

QString Proposal::hash() const { return QString::fromStdString(govObj.GetHash().ToString()); }

QDateTime Proposal::startDate() const { return m_startDate; }

QDateTime Proposal::endDate() const { return m_endDate; }

float Proposal::paymentAmount() const { return m_paymentAmount; }

QString Proposal::url() const { return m_url; }

bool Proposal::isActive() const
{
    LOCK(cs_main);
    std::string strError;
    return govObj.IsValidLocally(strError, false);
}

QString Proposal::votingStatus(const int nAbsVoteReq) const
{
    const int absYesCount = govObj.GetAbsoluteYesCount(VOTE_SIGNAL_FUNDING);
    if (absYesCount >= nAbsVoteReq) {
        return tr("Passing +%1").arg(absYesCount - nAbsVoteReq);
    } else {
        return tr("Needs additional %1 votes").arg(nAbsVoteReq - absYesCount);
    }
}

int Proposal::GetAbsoluteYesCount() const
{
    return govObj.GetAbsoluteYesCount(VOTE_SIGNAL_FUNDING);
}

int Proposal::GetYesCount() const
{
    return govObj.GetYesCount(VOTE_SIGNAL_FUNDING);
}

int Proposal::GetNoCount() const
{
    return govObj.GetNoCount(VOTE_SIGNAL_FUNDING);
}

int Proposal::GetAbstainCount() const
{
    return govObj.GetAbstainCount(VOTE_SIGNAL_FUNDING);
}

void Proposal::openUrl() const
{
    QDesktopServices::openUrl(QUrl(m_url));
}

QString Proposal::toJson() const
{
    const auto json = govObj.ToJson();
    return QString::fromStdString(json.write(2));
}

///
/// Proposal Model
///


int ProposalModel::rowCount(const QModelIndex& index) const
{
    return m_data.count();
}

int ProposalModel::columnCount(const QModelIndex& index) const
{
    return Column::_COUNT;
}

QVariant ProposalModel::data(const QModelIndex& index, int role) const
{
    if (role != Qt::DisplayRole && role != Qt::EditRole) return {};
    const auto proposal = m_data[index.row()];
    switch(role) {
    case Qt::DisplayRole:
    {
        switch (index.column()) {
        case Column::HASH:
            return proposal->hash();
        case Column::TITLE:
            return proposal->title();
        case Column::START_DATE:
            return proposal->startDate().date();
        case Column::END_DATE:
            return proposal->endDate().date();
        case Column::PAYMENT_AMOUNT:
            return proposal->paymentAmount();
        case Column::YES_VOTES:
            return proposal->GetYesCount();
        case Column::NO_VOTES:
            return proposal->GetNoCount();
        case Column::ABSTAIN_VOTES:
            return proposal->GetAbstainCount();
        case Column::IS_ACTIVE:
            return proposal->isActive() ? tr("Yes") : tr("No");
        case Column::VOTING_STATUS:
            return proposal->votingStatus(nAbsVoteReq);
        default:
            return {};
        };
        break;
    }
    case Qt::EditRole:
    {
        // Edit role is used for sorting, so return the raw values where possible
        switch (index.column()) {
        case Column::HASH:
            return proposal->hash();
        case Column::TITLE:
            return proposal->title();
        case Column::START_DATE:
            return proposal->startDate();
        case Column::END_DATE:
            return proposal->endDate();
        case Column::PAYMENT_AMOUNT:
            return proposal->paymentAmount();
        case Column::YES_VOTES:
            return proposal->GetYesCount();
        case Column::NO_VOTES:
            return proposal->GetNoCount();
        case Column::ABSTAIN_VOTES:
            return proposal->GetAbstainCount();
        case Column::IS_ACTIVE:
            return proposal->isActive();
        case Column::VOTING_STATUS:
            return proposal->GetAbsoluteYesCount();
        default:
            return {};
        };
        break;
    }
    };
    return {};
}

QVariant ProposalModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
    case Column::HASH:
        return tr("Hash");
    case Column::TITLE:
        return tr("Title");
    case Column::START_DATE:
        return tr("Start");
    case Column::END_DATE:
        return tr("End");
    case Column::PAYMENT_AMOUNT:
        return tr("Amount");
    case Column::YES_VOTES:
        return tr("Yes");
    case Column::NO_VOTES:
        return tr("No");
    case Column::ABSTAIN_VOTES:
        return tr("Abstain");
    case Column::IS_ACTIVE:
        return tr("Active");
    case Column::VOTING_STATUS:
        return tr("Status");
    default:
        return {};
    }
}

int ProposalModel::columnWidth(int section)
{
    switch (section) {
    case Column::HASH:
        return 80;
    case Column::TITLE:
        return 180;
    case Column::START_DATE:
    case Column::END_DATE:
        return 90;
    case Column::PAYMENT_AMOUNT:
        return 90;
    case Column::YES_VOTES:
    case Column::NO_VOTES:
    case Column::ABSTAIN_VOTES:
        return 55;
    case Column::IS_ACTIVE:
        return 60;
    case Column::VOTING_STATUS:
        return 180;
    default:
        return 80;
    }
}

void ProposalModel::append(const Proposal* proposal)
{
    beginInsertRows({}, m_data.count(), m_data.count());
    m_data.append(proposal);
    endInsertRows();
}

void ProposalModel::remove(int row)
{
    beginRemoveRows({}, row, row);
    delete m_data.at(row);
    m_data.removeAt(row);
    endRemoveRows();
}

void ProposalModel::reconcile(const std::vector<const Proposal*>& proposals)
{
    std::vector<bool> keep_index(m_data.count(), false);
    for (const auto proposal : proposals) {
        bool found = false;
        for (int i = 0; i < m_data.count(); ++i) {
            if (m_data.at(i)->hash() == proposal->hash()) {
                found = true;
                keep_index.at(i) = true;
                if (m_data.at(i)->GetAbsoluteYesCount() != proposal->GetAbsoluteYesCount() ||
                    m_data.at(i)->GetYesCount() != proposal->GetYesCount() ||
                    m_data.at(i)->GetNoCount() != proposal->GetNoCount() ||
                    m_data.at(i)->GetAbstainCount() != proposal->GetAbstainCount()) {
                    // replace proposal to update vote counts
                    delete m_data.at(i);
                    m_data.replace(i, proposal);
                    Q_EMIT dataChanged(createIndex(i, Column::YES_VOTES), createIndex(i, Column::VOTING_STATUS));
                } else {
                    // no changes
                    delete proposal;
                }
                break;
            }
        }
        if (!found) {
            append(proposal);
        }
    }
    for (unsigned int i = keep_index.size(); i > 0; --i) {
        if (!keep_index.at(i - 1)) {
            remove(i - 1);
        }
    }
}


void ProposalModel::setVotingParams(int newAbsVoteReq)
{
    if (this->nAbsVoteReq != newAbsVoteReq) {
        this->nAbsVoteReq = newAbsVoteReq;
        Q_EMIT dataChanged(createIndex(0, Column::VOTING_STATUS), createIndex(rowCount(), Column::VOTING_STATUS));
    }
}

const Proposal* ProposalModel::getProposalAt(const QModelIndex& index) const
{
    return m_data[index.row()];
}

//
// Governance Tab main widget.
//

GovernanceList::GovernanceList(QWidget* parent) :
    QWidget(parent),
    ui(std::make_unique<Ui::GovernanceList>()),
    proposalModel(new ProposalModel(this)),
    proposalModelProxy(new QSortFilterProxyModel(this)),
    proposalContextMenu(new QMenu(this)),
    timer(new QTimer(this))
{
    ui->setupUi(this);

    GUIUtil::setFont({ui->label_count_2, ui->countLabel}, GUIUtil::FontWeight::Bold, 14);
    GUIUtil::setFont({ui->label_filter_2}, GUIUtil::FontWeight::Normal, 15);

    proposalModelProxy->setSourceModel(proposalModel);
    ui->govTableView->setModel(proposalModelProxy);
    ui->govTableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->govTableView->horizontalHeader()->setStretchLastSection(true);

    for (int i = 0; i < proposalModel->columnCount(); ++i) {
        ui->govTableView->setColumnWidth(i, proposalModel->columnWidth(i));
    }

    // Set up sorting.
    proposalModelProxy->setSortRole(Qt::EditRole);
    ui->govTableView->setSortingEnabled(true);
    ui->govTableView->sortByColumn(ProposalModel::Column::START_DATE, Qt::DescendingOrder);

    // Set up filtering.
    proposalModelProxy->setFilterKeyColumn(ProposalModel::Column::TITLE);
    connect(ui->filterLineEdit, &QLineEdit::textChanged, proposalModelProxy, &QSortFilterProxyModel::setFilterFixedString);

    // Changes to number of rows should update proposal count display.
    connect(proposalModelProxy, &QSortFilterProxyModel::rowsInserted, this, &GovernanceList::updateProposalCount);
    connect(proposalModelProxy, &QSortFilterProxyModel::rowsRemoved, this, &GovernanceList::updateProposalCount);
    connect(proposalModelProxy, &QSortFilterProxyModel::layoutChanged, this, &GovernanceList::updateProposalCount);

    // Enable CustomContextMenu on the table.
    ui->govTableView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(ui->govTableView, &QTableView::customContextMenuRequested, this, &GovernanceList::showProposalContextMenu);
    connect(ui->govTableView, &QTableView::doubleClicked, this, &GovernanceList::showAdditionalInfo);

    // Vote buttons.
    connect(ui->voteYesButton, &QPushButton::clicked, this, &GovernanceList::onVoteYesClicked);
    connect(ui->voteNoButton, &QPushButton::clicked, this, &GovernanceList::onVoteNoClicked);
    connect(ui->voteAbstainButton, &QPushButton::clicked, this, &GovernanceList::onVoteAbstainClicked);

    // Create proposal button.
    connect(ui->createProposalButton, &QPushButton::clicked, this, &GovernanceList::onCreateProposalClicked);

    connect(timer, &QTimer::timeout, this, &GovernanceList::updateProposalList);

    GUIUtil::updateFonts();
}

GovernanceList::~GovernanceList() = default;

void GovernanceList::setClientModel(ClientModel* model)
{
    this->clientModel = model;
    updateProposalList();
}

void GovernanceList::setWalletModel(WalletModel* model)
{
    this->walletModel = model;
}

bool GovernanceList::executeRpc(const std::string& command, std::string& result)
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

void GovernanceList::voteOnProposal(const QString& outcome)
{
    if (!walletModel) {
        QMessageBox::warning(this, tr("Voting"), tr("Wallet not available."));
        return;
    }

    // Get selected proposal.
    QModelIndexList selection = ui->govTableView->selectionModel()->selectedRows();
    if (selection.empty()) {
        QMessageBox::warning(this, tr("Voting"), tr("Please select a proposal to vote on."));
        return;
    }

    const auto sourceIndex = proposalModelProxy->mapToSource(selection.first());
    const auto proposal = proposalModel->getProposalAt(sourceIndex);
    if (!proposal) return;

    // Confirm.
    QMessageBox::StandardButton reply = QMessageBox::question(
        this, tr("Confirm Vote"),
        tr("Vote <b>%1</b> on proposal <b>%2</b> with all your masternodes?")
            .arg(outcome, proposal->title()),
        QMessageBox::Yes | QMessageBox::Cancel);

    if (reply != QMessageBox::Yes) return;

    // Execute: gobject vote-many <hash> funding <yes|no|abstain>
    std::string result;
    std::string cmd = "gobject vote-many " + proposal->hash().toStdString() + " funding " + outcome.toStdString();
    bool ok = executeRpc(cmd, result);

    if (ok) {
        // Parse result to show success/failure count.
        UniValue resultObj(UniValue::VOBJ);
        if (resultObj.read(result)) {
            UniValue overall = find_value(resultObj, "overall");
            if (overall.isStr()) {
                QMessageBox::information(this, tr("Voting"), QString::fromStdString(overall.get_str()));
            } else {
                QMessageBox::information(this, tr("Voting"), QString::fromStdString(result));
            }
        } else {
            QMessageBox::information(this, tr("Voting"), QString::fromStdString(result));
        }
    } else {
        QMessageBox::critical(this, tr("Voting Failed"), QString::fromStdString(result));
    }

    // Refresh after voting.
    updateProposalList();
}

void GovernanceList::onVoteYesClicked()
{
    voteOnProposal("yes");
}

void GovernanceList::onVoteNoClicked()
{
    voteOnProposal("no");
}

void GovernanceList::onVoteAbstainClicked()
{
    voteOnProposal("abstain");
}

void GovernanceList::onCreateProposalClicked()
{
    if (!walletModel) {
        QMessageBox::warning(this, tr("Create Proposal"), tr("Wallet not available."));
        return;
    }

    ProposalDialog dlg(walletModel, this);
    dlg.exec();

    // Refresh list in case proposal was submitted.
    updateProposalList();
}

void GovernanceList::updateProposalList()
{
    if (this->clientModel) {
        const int nWeightedMnCount = clientModel->getMasternodeList().GetValidWeightedMNsCount();
        const int nAbsVoteReq = std::max(Params().GetConsensus().nGovernanceMinQuorum, nWeightedMnCount / 10);
        proposalModel->setVotingParams(nAbsVoteReq);

        std::vector<CGovernanceObject> govObjList;
        clientModel->getAllGovernanceObjects(govObjList);
        std::vector<const Proposal*> newProposals;
        for (const auto& govObj : govObjList) {
            if (govObj.GetObjectType() != GOVERNANCE_OBJECT_PROPOSAL) {
                continue;
            }

            newProposals.emplace_back(new Proposal(govObj, proposalModel));
        }
        proposalModel->reconcile(newProposals);
    }

    // Schedule next update.
    timer->start(GOVERNANCELIST_UPDATE_SECONDS * 1000);
}

void GovernanceList::updateProposalCount() const
{
    ui->countLabel->setText(QString::number(proposalModelProxy->rowCount()));
}

void GovernanceList::showProposalContextMenu(const QPoint& pos)
{
    const auto index = ui->govTableView->indexAt(pos);

    if (!index.isValid()) {
        return;
    }

    const auto proposal = proposalModel->getProposalAt(proposalModelProxy->mapToSource(index));
    if (proposal == nullptr) {
        return;
    }

    proposalContextMenu->clear();

    // Open URL action.
    if (!proposal->url().isEmpty()) {
        QAction* openProposalUrl = new QAction(tr("Open proposal URL"), this);
        proposalContextMenu->addAction(openProposalUrl);
        connect(openProposalUrl, &QAction::triggered, proposal, &Proposal::openUrl);
    }

    // Copy hash action.
    QAction* copyHash = new QAction(tr("Copy proposal hash"), this);
    proposalContextMenu->addAction(copyHash);
    connect(copyHash, &QAction::triggered, this, [proposal]() {
        QApplication::clipboard()->setText(proposal->hash());
    });

    proposalContextMenu->addSeparator();

    // Vote actions.
    QAction* voteYes = new QAction(tr("Vote Yes"), this);
    QAction* voteNo = new QAction(tr("Vote No"), this);
    QAction* voteAbstain = new QAction(tr("Vote Abstain"), this);
    proposalContextMenu->addAction(voteYes);
    proposalContextMenu->addAction(voteAbstain);
    proposalContextMenu->addAction(voteNo);
    connect(voteYes, &QAction::triggered, this, &GovernanceList::onVoteYesClicked);
    connect(voteNo, &QAction::triggered, this, &GovernanceList::onVoteNoClicked);
    connect(voteAbstain, &QAction::triggered, this, &GovernanceList::onVoteAbstainClicked);

    proposalContextMenu->exec(QCursor::pos());
}

void GovernanceList::showAdditionalInfo(const QModelIndex& index)
{
    if (!index.isValid()) {
        return;
    }

    const auto proposal = proposalModel->getProposalAt(proposalModelProxy->mapToSource(index));
    if (proposal == nullptr) {
        return;
    }

    const auto windowTitle = tr("Proposal Info: %1").arg(proposal->title());
    const auto json = proposal->toJson();

    QMessageBox::information(this, windowTitle, json);
}
