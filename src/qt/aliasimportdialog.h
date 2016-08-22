#ifndef ALIASIMPORTDIALOG_H
#define ALIASIMPORTDIALOG_H
#include <QDialog>
class PlatformStyle;
class QDataWidgetMapper;
class UniValue;
namespace Ui {
    class AliasImportDialog;
}
QT_BEGIN_NAMESPACE
class QModelIndex;
class QStandardItemModel;
QT_END_NAMESPACE
/** Dialog for editing an address and associated information.
 */
class AliasImportDialog : public QDialog
{
    Q_OBJECT

public:
    explicit AliasImportDialog(const PlatformStyle *platformStyle, const QModelIndex &idx, QWidget *parent=0);
    ~AliasImportDialog();
    void addParentItem(QStandardItemModel * model, const QString& text, const QVariant& data );
    void addChildItem(QStandardItemModel * model, const QString& text, const QVariant& data );
private Q_SLOTS:
	void on_cancelButton_clicked();
	void on_importCerts_clicked();
	void on_importOffers_clicked();
	void on_importEscrows_clicked();
private:
	void loadCategories();
	QDataWidgetMapper *mapper;
    Ui::AliasImportDialog *ui;
	QString alias;

};

#endif // ALIASIMPORTDIALOG_H
