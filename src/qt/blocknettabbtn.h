// Copyright (c) 2018-2019 The Blocknet developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BLOCKNETTABBTN_H
#define BLOCKNETTABBTN_H

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

class BlocknetTabBtn : public QPushButton
{
    Q_OBJECT
public:
    explicit BlocknetTabBtn(QPushButton *parent = nullptr);

protected:
    bool event(QEvent *event) override;

private:
    QVBoxLayout *layout;
    QLabel *subLine;
};

#endif // BLOCKNETTABBTN_H