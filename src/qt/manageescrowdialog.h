#ifndef MANAGEESCROWDIALOG_H
#define MANAGEESCROWDIALOG_H

#include <QDialog>
class WalletModel;
namespace Ui {
    class ManageEscrowDialog;
}
QT_BEGIN_NAMESPACE
class QNetworkReply;
QT_END_NAMESPACE
/** Dialog for editing an address and associated information.
 */
 enum EscrowRoleType {
    Buyer,
    Seller,
	Arbiter,
	None
};
class ManageEscrowDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ManageEscrowDialog(WalletModel* model, const QString &escrow, QWidget *parent = 0);
    ~ManageEscrowDialog();
	void SendRawTxBTC();
	void CheckPaymentInBTC();
	bool isYourAlias(const QString &alias);
	bool CompleteEscrowRefund();
	bool CompleteEscrowRelease();
	bool loadEscrow(const QString &escrow, QString &buyer, QString &seller, QString &arbiter, QString &status, QString &offertitle, QString &total, QString &btctxid, QString &redeemtxid);
	QString EscrowRoleTypeToString(const EscrowRoleType& escrowType);
	EscrowRoleType findYourEscrowRoleFromAliases(const QString &buyer, const QString &seller, const QString &arbiter);
	EscrowRoleType escrowRoleType;
public Q_SLOTS:
	void on_releaseButton_clicked();
	void on_btcButton_clicked();
	void on_refundButton_clicked();
	void on_cancelButton_clicked();
	void slotConfirmedFinished(QNetworkReply *);
	void slotConfirmedFinishedCheck(QNetworkReply *);
private:
	void onLeaveFeedback();
	WalletModel* walletModel;
    Ui::ManageEscrowDialog *ui;
	QString escrow;
	QString m_btctxid;
	QString m_redeemTxId;
	QString m_rawTx;
	QString refundWarningStr;
	QString releaseWarningStr;
	QString m_buttontext;
};

#endif // MANAGEESCROWDIALOG_H
