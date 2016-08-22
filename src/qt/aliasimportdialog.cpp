#include "aliasimportdialog.h"
#include "ui_aliasimportdialog.h"
#include "aliastablemodel.h"
#include "init.h"
#include "util.h"
#include "offer.h"
#include "guiutil.h"
#include "syscoingui.h"
#include "platformstyle.h"
#include <QMessageBox>
#include <QModelIndex>
#include <QDateTime>
#include <QDataWidgetMapper>
#include <QLineEdit>
#include <QTextEdit>
#include <QGroupBox>
#include <QLabel>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include "rpcserver.h"
#include <QStandardItemModel>
#include "qcomboboxdelegate.h"
#include <boost/algorithm/string.hpp>
using namespace std;

extern const CRPCTable tableRPC;

AliasImportDialog::AliasImportDialog(const PlatformStyle *platformStyle, const QModelIndex &idx, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::AliasImportDialog)
{
    ui->setupUi(this);

    mapper = new QDataWidgetMapper(this);
    mapper->setSubmitPolicy(QDataWidgetMapper::ManualSubmit);
	alias = idx.data(AliasTableModel::NameRole).toString();
	ui->importOfferDisclaimer->setText(tr("<font color='blue'>You may import your offers related to alias <b>%1</b>. This is useful if the alias has been transferred to you and you wish to own offers created with the alias. Below you may filter your import criteria based on offer category and/or the offers being safe to search.</font>").arg(alias));	
	ui->importCertDisclaimer->setText(tr("<font color='blue'>You may import your certificates related to alias <b>%1</b>. This is useful if the alias has been transferred to you and you wish to own certificates created with the alias. Below you may filter your import criteria based on certificate category and/or the certificates being safe to search.</font>").arg(alias));	
	ui->importEscrowDisclaimer->setText(tr("<font color='blue'>You may import your escrows related to alias <b>%1</b>. This is useful if the alias has been transferred to you and you wish to own escrows created with the alias.</font>").arg(alias));	
	QString theme = GUIUtil::getThemeName();  
	if (!platformStyle->getImagesOnButtons())
	{
		ui->cancelButton->setIcon(QIcon());
		ui->importOffers->setIcon(QIcon());
		ui->importCerts->setIcon(QIcon());
		ui->importEscrows->setIcon(QIcon());
	}
	else
	{
		ui->cancelButton->setIcon(platformStyle->SingleColorIcon(":/icons/" + theme + "/quit"));
		ui->importOffers->setIcon(platformStyle->SingleColorIcon(":/icons/" + theme + "/add"));
		ui->importCerts->setIcon(platformStyle->SingleColorIcon(":/icons/" + theme + "/add"));
		ui->importEscrows->setIcon(platformStyle->SingleColorIcon(":/icons/" + theme + "/add"));
	}
	loadCategories();
}

AliasImportDialog::~AliasImportDialog()
{
    delete ui;
}

void AliasImportDialog::addParentItem( QStandardItemModel * model, const QString& text, const QVariant& data )
{
	QList<QStandardItem*> lst = model->findItems(text,Qt::MatchExactly);
	for(unsigned int i=0; i<lst.count(); ++i )
	{ 
		if(lst[i]->data(Qt::UserRole) == data)
			return;
	}
    QStandardItem* item = new QStandardItem( text );
	item->setData( data, Qt::UserRole );
    item->setData( "parent", Qt::AccessibleDescriptionRole );
    QFont font = item->font();
    font.setBold( true );
    item->setFont( font );
    model->appendRow( item );
}

void OfferListPage::addChildItem( QStandardItemModel * model, const QString& text, const QVariant& data )
{
	QList<QStandardItem*> lst = model->findItems(text,Qt::MatchExactly);
	for(unsigned int i=0; i<lst.count(); ++i )
	{ 
		if(lst[i]->data(Qt::UserRole) == data)
			return;
	}

    QStandardItem* item = new QStandardItem( text + QString( 4, QChar( ' ' ) ) );
    item->setData( data, Qt::UserRole );
    item->setData( "child", Qt::AccessibleDescriptionRole );
    model->appendRow( item );
}
void OfferListPage::loadCategories()
{
    QStandardItemModel * offermodel = new QStandardItemModel;
	QStandardItemModel * certmodel = new QStandardItemModel;
	vector<string> categoryList;
	if(!getCategoryList(categoryList))
	{
		return;
	}
	addParentItem(offermodel, tr("All Categories"), tr("All Categories"));
	for(unsigned int i = 0;i< categoryList.size(); i++)
	{
		vector<string> categories;
		boost::split(categories,categoryList[i],boost::is_any_of(">"));
		if(categories.size() > 0)
		{
			for(unsigned int j = 0;j< categories.size(); j++)
			{
				boost::algorithm::trim(categories[j]);
				// only support 2 levels in qt GUI for categories
				if(j == 0)
				{
					addParentItem(offermodel, QString::fromStdString(categories[0]), QVariant(QString::fromStdString(categories[0])));
				}
				else if(j == 1)
				{
					addChildItem(offermodel, QString::fromStdString(categories[1]), QVariant(QString::fromStdString(categoryList[i])));
				}
			}
		}
		else
		{
			addParentItem(offermodel, QString::fromStdString(categoryList[i]), QVariant(QString::fromStdString(categoryList[i])));
		}
	}
    ui->offerCategory->setModel(offermodel);
    ui->offerCategory->setItemDelegate(new ComboBoxDelegate);

	addParentItem(certmodel, tr("All Certificates"), tr("certificates"));
	for(unsigned int i = 0;i< categoryList.size(); i++)
	{
		vector<string> categories;
		boost::split(categories,categoryList[i],boost::is_any_of(">"));
		if(categories.size() > 0)
		{
			for(unsigned int j = 0;j< categories.size(); j++)
			{
				boost::algorithm::trim(categories[j]);
				if(categories[0] != "certificates")
					continue;
				if(j == 1)
				{
					addChildItem(certmodel, QString::fromStdString(categories[1]), QVariant(QString::fromStdString(categoryList[i])));
				}
			}
		}
	}
    ui->certCategory->setModel(certmodel);
    ui->certCategory->setItemDelegate(new ComboBoxDelegate);

}

void AliasImportDialog::on_cancelButton_clicked()
{
    mapper->submit();
    accept();
}
bool AliasImportDialog::on_importOffers_clicked()
{
	string strMethod = string("importoffersusedbyalias");
	UniValue params(UniValue::VARR);
	UniValue result;
	params.push_back(alias.toStdString());
	QVariant currentCategory = ui->offerCategories->itemData(ui->offerCategories->currentIndex(), Qt::UserRole);
	if(ui->offerCategories->currentIndex() > 0 &&  currentCategory != QVariant::Invalid)
		params.push_back(currentCategory.toString().toStdString());
	else if(ui->offerCategories->currentText() != tr("All Categories"))
		params.push_back(ui->offerCategories->currentText().toStdString());
	params.push_back(ui->offerSafeSearch->checkState() == QT::Checked? "Yes": "No");
    try {
        result = tableRPC.execute(strMethod, params);
		QMessageBox::information(this, windowTitle(),
				tr("%1 Offers have been imported into your wallet!").arg(result.size()),
				QMessageBox::Ok, QMessageBox::Ok);
	}
	catch (UniValue& objError)
	{
		QMessageBox::critical(this, windowTitle(),
				tr("Could not find this any offers related to this alias"),
				QMessageBox::Ok, QMessageBox::Ok);

	}
	catch(std::exception& e)
	{
		QMessageBox::critical(this, windowTitle(),
			tr("There was an exception trying to locate offers related to this alias: ") + QString::fromStdString(e.what()),
				QMessageBox::Ok, QMessageBox::Ok);
	}
	return false;


}
bool AliasImportDialog::on_importCerts_clicked()
{
	string strMethod = string("importcertsusedbyalias");
	UniValue params(UniValue::VARR);
	UniValue result;
	params.push_back(alias.toStdString());
	QVariant currentCategory = ui->certCategories->itemData(ui->certCategories->currentIndex(), Qt::UserRole);
	if(ui->certCategories->currentIndex() > 0 &&  currentCategory != QVariant::Invalid)
		params.push_back(currentCategory.toString().toStdString());
	else if(ui->certCategories->currentText() != tr("All Certificates"))
		params.push_back(ui->certCategories->currentText().toStdString());
	params.push_back(ui->certSafeSearch->checkState() == QT::Checked? "Yes": "No");
    try {
        result = tableRPC.execute(strMethod, params);
		QMessageBox::information(this, windowTitle(),
				tr("%1 Certificates have been imported into your wallet!").arg(result.size()),
				QMessageBox::Ok, QMessageBox::Ok);
	}
	catch (UniValue& objError)
	{
		QMessageBox::critical(this, windowTitle(),
				tr("Could not find this any certificates related to this alias"),
				QMessageBox::Ok, QMessageBox::Ok);

	}
	catch(std::exception& e)
	{
		QMessageBox::critical(this, windowTitle(),
			tr("There was an exception trying to locate offers related to this alias: ") + QString::fromStdString(e.what()),
				QMessageBox::Ok, QMessageBox::Ok);
	}
	return false;
}
bool AliasImportDialog::on_importEscrows_clicked()
{
	string strMethod = string("importescrowsusedbyalias");
	UniValue params(UniValue::VARR);
	UniValue result;
	params.push_back(alias.toStdString());
    try {
        result = tableRPC.execute(strMethod, params);
		QMessageBox::information(this, windowTitle(),
				tr("%1 Escrows have been imported into your wallet!").arg(result.size()),
				QMessageBox::Ok, QMessageBox::Ok);
	}
	catch (UniValue& objError)
	{
		QMessageBox::critical(this, windowTitle(),
				tr("Could not find this any escrows related to this alias"),
				QMessageBox::Ok, QMessageBox::Ok);

	}
	catch(std::exception& e)
	{
		QMessageBox::critical(this, windowTitle(),
			tr("There was an exception trying to locate escrows related to this alias: ") + QString::fromStdString(e.what()),
				QMessageBox::Ok, QMessageBox::Ok);
	}
	return false;
}



