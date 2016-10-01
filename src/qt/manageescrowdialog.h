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
class ManageEscrowDialog : public QDialog
{
    Q_OBJECT

public:
    enum EscrowType {
        Buyer,
        Seller,
		Arbiter,
		None
    };
    explicit ManageEscrowDialog(WalletModel* model, const QString &escrow, QWidget *parent = 0);
    ~ManageEscrowDialog();
	void SendRawTxBTC();
	void CheckPaymentInBTC();
	bool isYourAlias(const QString &alias);
	void CompleteEscrowRefund(const QString& timestamp="");
	void CompleteEscrowRelease(const QString& timestamp="");
	bool loadEscrow(const QString &escrow, QString &buyer, QString &seller, QString &arbiter, QString &status, QString &offertitle, QString &total);
	ManageEscrowDialog::EscrowType findYourEscrowRoleFromAliases(const QString &buyer, const QString &seller, const QString &arbiter);
public Q_SLOTS:
	void on_releaseButton_clicked();
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
	QString m_redeemTxid;
	QString m_rawTx;
	QString refundWarningStr;
	QString releaseWarningStr;
};

#endif // MANAGEESCROWDIALOG_H
