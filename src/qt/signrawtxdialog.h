#ifndef SIGNRAWTXDIALOG_H
#define SIGNRAWTXDIALOG_H

#include <QDialog>

namespace Ui {
    class SignRawTxDialog;
}
QT_BEGIN_NAMESPACE
QT_END_NAMESPACE

/** Dialog for editing an address and associated information.
 */
class SignRawTxDialog : public QDialog
{
    Q_OBJECT

public:

    explicit SignRawTxDialog();
    ~SignRawTxDialog();


public Q_SLOTS:
    void accept();
};

#endif // SIGNRAWTXDIALOG_H
