// Copyright (c) 2024 The OmegaCoin developers
// Distributed under the MIT/X11 software licence, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_SNAPSHOTRECOVERY_H
#define BITCOIN_QT_SNAPSHOTRECOVERY_H

#include <QAtomicInt>
#include <QDialog>
#include <QSemaphore>
#include <QString>
#include <boost/filesystem/path.hpp>

class QLabel;
class QProgressBar;
class QPushButton;
class QCloseEvent;
class QEvent;
class QKeyEvent;
class SnapshotRecoveryThread;

class SnapshotRecoveryDialog : public QDialog
{
public:
    explicit SnapshotRecoveryDialog(const boost::filesystem::path& dataDir, QWidget* parent = 0);
    ~SnapshotRecoveryDialog();
    bool recoverySucceeded() const { return m_success; }
    bool recoveryCancelled() const { return m_cancelled; }
    QString failureReason() const { return m_failureReason; }
    void reject();

protected:
    void closeEvent(QCloseEvent* event);
    bool event(QEvent* event);
    void keyPressEvent(QKeyEvent* event);

private:
    boost::filesystem::path m_dataDir;
    QLabel* m_statusLabel;
    QProgressBar* m_progressBar;
    QPushButton* m_cancelButton;
    SnapshotRecoveryThread* m_worker;
    QAtomicInt m_cancelRequested;
    QSemaphore m_decisionSemaphore;
    QAtomicInt m_userDecision;
    bool m_success;
    bool m_cancelled;
    QString m_failureReason;
};

#endif // BITCOIN_QT_SNAPSHOTRECOVERY_H
