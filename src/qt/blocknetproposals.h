// Copyright (c) 2018-2019 The Blocknet developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BLOCKNETPROPOSALS_H
#define BLOCKNETPROPOSALS_H

#include <qt/blocknetdropdown.h>
#include <qt/blocknetvars.h>

#include <qt/walletmodel.h>

#include <governance/governance.h>
#include <validation.h>

#include <QDateTime>
#include <QDialog>
#include <QFrame>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTimer>
#include <QVBoxLayout>
#include <QVector>

class BlocknetProposals : public QFrame
{
    Q_OBJECT

public:
    explicit BlocknetProposals(QFrame *parent = nullptr);
    void setWalletModel(WalletModel *w);

    enum statusflags {
        STATUS_PASSED = 0,
        STATUS_IN_PROGRESS = 1,
        STATUS_REJECTED = 2,
    };

    struct BlocknetProposal {
        uint256 hash;
        statusflags color;
        QString name;
        int superblock;
        CAmount amount;
        QString url;
        QString description;
        QString status;
        QString results;
        gov::VoteType vote;
        QString voteString;
        CAmount voteAmount;
    };

    void clear() {
        if (dataModel.count() > 0)
            table->clearContents();
    };

    QTableWidget* getTable() {
        return table;
    }

Q_SIGNALS:
    void createProposal();
    void tableUpdated();

public Q_SLOTS:
    void onCreateProposal() {
        Q_EMIT createProposal();
    }
    void onVote();

private Q_SLOTS:
    void onItemChanged(QTableWidgetItem *item);
    void onFilter();
    void showProposalDetails(const BlocknetProposal & proposal);

private:
    int getChainHeight() const {
        int height{std::numeric_limits<int>::max()};
        {
            LOCK(cs_main);
            height = chainActive.Height();
        }
        return height;
    }

private:
    QVBoxLayout *layout;
    WalletModel *walletModel;
    QLabel *titleLbl;
    QLabel *buttonLbl;
    QLabel *filterLbl;
    QTableWidget *table;
    QMenu *contextMenu;
    QTableWidgetItem *contextItem = nullptr;
    BlocknetDropdown *proposalsDropdown;
    QVector<BlocknetProposal> dataModel;
    QVector<BlocknetProposal> filteredData;
    QTimer *timer;
    int lastRow = -1;
    qint64 lastSelection = 0;
    bool syncInProgress = false;

    void initialize();
    void setData(QVector<BlocknetProposal> data);
    QVector<BlocknetProposal> filtered(int filter, int chainHeight);
    void unwatch();
    void watch();
    bool canVote();
    void refresh(bool force = false);
    void showContextMenu(QPoint pt);

    enum {
        COLUMN_HASH,
        COLUMN_COLOR,
        COLUMN_PADDING1,
        COLUMN_NAME,
        COLUMN_SUPERBLOCK,
        COLUMN_AMOUNT,
        COLUMN_URL,
        COLUMN_DESCRIPTION,
        COLUMN_STATUS,
        COLUMN_RESULTS,
        COLUMN_VOTE,
        COLUMN_PADDING2
    };

    enum {
        FILTER_ALL,
        FILTER_ACTIVE,
        FILTER_UPCOMING,
        FILTER_COMPLETED
    };

    class NumberItem : public QTableWidgetItem {
    public:
        explicit NumberItem() = default;
        bool operator < (const QTableWidgetItem &other) const override {
            return amount < reinterpret_cast<const NumberItem*>(&other)->amount;
        };
        CAmount amount{0};
    };
};

class BlocknetProposalsVoteDialog : public QDialog {
    Q_OBJECT
public:
    explicit BlocknetProposalsVoteDialog(const BlocknetProposals::BlocknetProposal & proposal, int displayUnit, QWidget *parent = nullptr);

Q_SIGNALS:
    void submitVote(uint256 proposalHash, bool yes, bool no, bool abstain);

protected:

private:
};

class BlocknetProposalsDetailsDialog : public QDialog {
    Q_OBJECT
public:
    explicit BlocknetProposalsDetailsDialog(const BlocknetProposals::BlocknetProposal & proposal, int displayUnit, QWidget *parent = nullptr);

protected:

private:
};

#endif // BLOCKNETPROPOSALS_H
