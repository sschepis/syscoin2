#ifndef SIGNRAWTXDIALOG_H
#define SIGNRAWTXDIALOG_H

#include <QDialog>

namespace Ui {
    class SignRawTxDialog;
}

/** Dialog for editing an address and associated information.
 */
class SignRawTxDialog : public QDialog
{
    Q_OBJECT

public:

    explicit SignRawTxDialog(QWidget *parent = 0);
    ~SignRawTxDialog();
private:
	bool saveCurrentRow();
	Ui::SignRawTxDialog *ui;

public Q_SLOTS:
    void accept();
};

#endif // SIGNRAWTXDIALOG_H
