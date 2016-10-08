#include "editaliasdialog.h"
#include "ui_editaliasdialog.h"

#include "aliastablemodel.h"
#include "guiutil.h"
#include "walletmodel.h"
#include "syscoingui.h"
#include "ui_interface.h"
#include <QDataWidgetMapper>
#include <QInputDialog>
#include <QMessageBox>
#include "rpc/server.h"
using namespace std;

extern CRPCTable tableRPC;
EditAliasDialog::EditAliasDialog(Mode mode, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::EditAliasDialog), mapper(0), mode(mode), model(0)
{
    ui->setupUi(this);

	ui->transferEdit->setVisible(false);
	ui->transferLabel->setVisible(false);
	ui->transferDisclaimer->setText(tr("<font color='blue'>Transfering your alias will also transfer ownership all of your syscoin services that use this alias, the new owner can use these services by clicking on import button from the alias list screen which will import alias key into their wallet</font>"));
	ui->transferDisclaimer->setVisible(false);
	ui->safeSearchDisclaimer->setText(tr("<font color='blue'>Is this alias safe to search? Anything that can be considered offensive to someone should be set to <b>No</b> here. If you do create an alias that is offensive and do not set this option to <b>No</b> your alias will be banned!</font>"));
	ui->expiryEdit->clear();
	ui->expiryEdit->addItem(tr("1 Year"),"1");
	ui->expiryEdit->addItem(tr("2 Years"),"2");
	ui->expiryEdit->addItem(tr("3 Years"),"3");
	ui->expiryEdit->addItem(tr("4 Years"),"4");
	ui->expiryEdit->addItem(tr("5 Years"),"5");
	ui->expiryDisclaimer->setText(tr("<font color='blue'>Set the length of time to keep your alias from expiring. The longer you wish to keep it alive the more fees you will pay to create or update this alias. The formula for the fee is 0.2 SYS * years * years.</font>"));
    ui->privateDisclaimer->setText(tr("<font color='blue'>This is to private profile information which is encrypted and only available to you. This is useful for when sending notes to a merchant through the payment screen so you don't have to type it out everytime.</font>"));
	ui->publicDisclaimer->setText(tr("<font color='blue'>This is public profile information that anyone on the network can see. Fill this in with things you would like others to know about you.</font>"));
	ui->multisigTitle->setText(tr("<font color='blue'>Set up your multisig alias here with the required number of signatures and the aliases that are capable of signing when this alias is updated. You may also sign a raw alias transaction below if you have been given one by a signee to complete a multisig transaction on this alias</font>"));
	connect(ui->rawTxBox,SIGNAL(clicked(bool)),SLOT(onSendRawTxChecked(bool)));
	connect(ui->reqSigsEdit, SIGNAL(textChanged(const QString &)), this, SLOT(reqSigsChanged()));
	ui->rawTxEdit->setEnabled(false);
	ui->reqSigsEdit->setValidator( new QIntValidator(0, 50, this) );
	switch(mode)
    {
    case NewDataAlias:
        setWindowTitle(tr("New Data Alias"));
		ui->rawTxBox->setEnabled(false);
        break;
    case NewAlias:
        setWindowTitle(tr("New Alias"));
		ui->rawTxBox->setEnabled(false);
        break;
    case EditDataAlias:
        setWindowTitle(tr("Edit Data Alias"));
		ui->aliasEdit->setEnabled(false);
        break;
    case EditAlias:
        setWindowTitle(tr("Edit Alias"));
		ui->aliasEdit->setEnabled(false);
        break;
    case TransferAlias:
        setWindowTitle(tr("Transfer Alias"));
		ui->aliasEdit->setEnabled(false);
		ui->nameEdit->setEnabled(false);
		ui->safeSearchEdit->setEnabled(false);
		ui->safeSearchDisclaimer->setVisible(false);
		ui->privateEdit->setEnabled(false);
		ui->transferEdit->setVisible(true);
		ui->transferLabel->setVisible(true);
		ui->transferDisclaimer->setVisible(true);
		ui->rawTxBox->setEnabled(false);
		ui->reqSigsEdit->setEnabled(false);
		ui->multisigList->setEnabled(false);
		ui->addButton->setEnabled(false);
		ui->deleteButton->setEnabled(false);
		ui->tabWidget->setCurrentIndex(1);
        break;
    }
    mapper = new QDataWidgetMapper(this);
    mapper->setSubmitPolicy(QDataWidgetMapper::ManualSubmit);
}

EditAliasDialog::~EditAliasDialog()
{
    delete ui;
}
void EditAliasDialog::onSendRawTxChecked(bool toggled)
{
	if(ui->rawTxBox->isChecked())
	{
		ui->nameEdit->setEnabled(false);
		ui->safeSearchEdit->setEnabled(false);
		ui->privateEdit->setEnabled(false);
		ui->transferEdit->setEnabled(false);
		ui->transferLabel->setEnabled(false);
		ui->transferDisclaimer->setEnabled(false);
		ui->reqSigsEdit->setEnabled(false);
		ui->multisigList->setEnabled(false);
		ui->addButton->setEnabled(false);
		ui->deleteButton->setEnabled(false);
		ui->expiryEdit->setEnabled(false);
		ui->rawTxEdit->setEnabled(true);	
	}
	else
	{
		ui->nameEdit->setEnabled(true);
		ui->safeSearchEdit->setEnabled(true);
		ui->privateEdit->setEnabled(true);
		ui->transferEdit->setEnabled(true);
		ui->transferLabel->setEnabled(true);
		ui->transferDisclaimer->setEnabled(true);
		ui->reqSigsEdit->setEnabled(true);
		ui->multisigList->setEnabled(true);
		ui->addButton->setEnabled(true);
		ui->deleteButton->setEnabled(true);
		ui->expiryEdit->setEnabled(true);
		ui->rawTxEdit->setEnabled(false);	
	}
}

void EditAliasDialog::on_cancelButton_clicked()
{
    reject();
}
void EditAliasDialog::on_addButton_clicked()
{
	
    bool ok;
    QString text = QInputDialog::getText(this, tr("Enter an alias"),
                                         tr("Alias:"), QLineEdit::Normal,
                                         "", &ok);
    if (ok && !text.isEmpty())
        ui->multisigList->addItem(text);

	ui->multisigDisclaimer->setText(tr("<font color='blue'>This is a <b>%1</b> of <b>%2</b> multisig alias</font>").arg(ui->reqSigsEdit->text()).arg(QString::number(ui->multisigList->count())));
}
void EditAliasDialog::on_deleteButton_clicked()
{
    QModelIndexList selected = ui->multisigList->selectionModel()->selectedIndexes();    
	for (int i = selected.count() - 1; i > -1; --i)
		ui->multisigList->model()->removeRow(selected.at(i).row());
	ui->multisigDisclaimer->setText(tr("<font color='blue'>This is a <b>%1</b> of <b>%2</b> multisig alias</font>").arg(ui->reqSigsEdit->text()).arg(QString::number(ui->multisigList->count())));
}
void EditAliasDialog::reqSigsChanged()
{
	ui->multisigDisclaimer->setText(tr("<font color='blue'>This is a <b>%1</b> of <b>%2</b> multisig alias</font>").arg(ui->reqSigsEdit->text()).arg(QString::number(ui->multisigList->count())));
}
void EditAliasDialog::on_okButton_clicked()
{
    mapper->submit();
    accept();
}
void EditAliasDialog::setModel(WalletModel* walletModel, AliasTableModel *model)
{
    this->model = model;
	this->walletModel = walletModel;
    if(!model) return;

    mapper->setModel(model);
	mapper->addMapping(ui->aliasEdit, AliasTableModel::Name);
    mapper->addMapping(ui->nameEdit, AliasTableModel::Value);
	mapper->addMapping(ui->privateEdit, AliasTableModel::PrivValue);
	
    
}

void EditAliasDialog::loadRow(int row)
{
    mapper->setCurrentIndex(row);
	const QModelIndex tmpIndex;
	if(model)
	{
		QModelIndex indexSafeSearch= model->index(row, AliasTableModel::SafeSearch, tmpIndex);
		if(indexSafeSearch.isValid())
		{
			QString safeSearchStr = indexSafeSearch.data(AliasTableModel::SafeSearchRole).toString();
			ui->safeSearchEdit->setCurrentIndex(ui->safeSearchEdit->findText(safeSearchStr));
		}
	}
}

bool EditAliasDialog::saveCurrentRow()
{
	UniValue params(UniValue::VARR);
	UniValue arraySendParams(UniValue::VARR);
	string strMethod;
	if(ui->rawTxBox->isChecked())
	{
		strMethod = string("syscoinsignrawtransaction");
		params.push_back(ui->rawTxEdit->toPlainText().toStdString());

		try {
            UniValue result = tableRPC.execute(strMethod, params);
			const UniValue& so = result.get_obj();
			string hex_str = "";

			const UniValue& hex_value = find_value(so, "hex");
			if (hex_value.isStr())
				hex_str = hex_value.get_str();
			const UniValue& complete_value = find_value(so, "complete");
			bool bComplete = false;
			if (complete_value.isStr())
				bComplete = complete_value.get_str() == "true";
			
			if(bComplete)
			{
				QMessageBox::information(this, windowTitle(),
					tr("Transaction was completed successfully!"),
						QMessageBox::Ok, QMessageBox::Ok);
			}
			else
			{
				GUIUtil::setClipboard(QString::fromStdString(hex_str));
				QMessageBox::critical(this, windowTitle(),
					tr("This transaction requires more signatures. Transaction hex <b>%1</b> has been copied to your clipboard for your reference. Please provide it to a signee that hasn't yet signed.").arg(QString::fromStdString(hex_str)),
						QMessageBox::Ok, QMessageBox::Ok);
			}

			return true;
		}
		catch (UniValue& objError)
		{
			string strError = find_value(objError, "message").get_str();
			QMessageBox::critical(this, windowTitle(),
			tr("Error creating updating multisig alias: \"%1\"").arg(QString::fromStdString(strError)),
				QMessageBox::Ok, QMessageBox::Ok);
		}
		catch(std::exception& e)
		{
			QMessageBox::critical(this, windowTitle(),
				tr("General exception creating sending raw alias update transaction"),
				QMessageBox::Ok, QMessageBox::Ok);
		}	
		return false;
	}
    if(!model || !walletModel) return false;
    WalletModel::UnlockContext ctx(walletModel->requestUnlock());
    if(!ctx.isValid())
    {
		model->editStatus = AliasTableModel::WALLET_UNLOCK_FAILURE;
        return false;
    }
    switch(mode)
    {
    case NewDataAlias:
    case NewAlias:
        if (ui->aliasEdit->text().trimmed().isEmpty()) {
            ui->aliasEdit->setText("");
            QMessageBox::information(this, windowTitle(),
            tr("Empty name for Alias not allowed. Please try again"),
                QMessageBox::Ok, QMessageBox::Ok);
            return false;
        }
		strMethod = string("aliasnew");
        params.push_back(ui->aliasEdit->text().trimmed().toStdString());
		params.push_back(ui->nameEdit->toPlainText().toStdString());
		params.push_back(ui->privateEdit->toPlainText().toStdString());
		params.push_back(ui->safeSearchEdit->currentText().toStdString());
		params.push_back(ui->expiryEdit->itemData(ui->expiryEdit->currentIndex()).toString().toStdString());
		if(ui->multisigList->count() > 0)
		{
			params.push_back(ui->reqSigsEdit->text().toStdString());
			for(int i = 0; i < ui->multisigList->count(); ++i)
			{
				QString str = ui->multisigList->item(i)->text();
				arraySendParams.push_back(str.toStdString());
			}
			params.push_back(arraySendParams);
		}

		try {
            UniValue result = tableRPC.execute(strMethod, params);
			if (result.type() != UniValue::VNULL)
			{
				alias = ui->nameEdit->toPlainText() + ui->aliasEdit->text();
					
			}
		}
		catch (UniValue& objError)
		{
			string strError = find_value(objError, "message").get_str();
			QMessageBox::critical(this, windowTitle(),
			tr("Error creating new Alias: \"%1\"").arg(QString::fromStdString(strError)),
				QMessageBox::Ok, QMessageBox::Ok);
			break;
		}
		catch(std::exception& e)
		{
			QMessageBox::critical(this, windowTitle(),
				tr("General exception creating new Alias"),
				QMessageBox::Ok, QMessageBox::Ok);
			break;
		}							

        break;
    case EditDataAlias:
    case EditAlias:
        if(mapper->submit())
        {
			strMethod = string("aliasupdate");
			params.push_back(ui->aliasEdit->text().toStdString());
			params.push_back(ui->nameEdit->toPlainText().toStdString());
			params.push_back(ui->privateEdit->toPlainText().toStdString());
			params.push_back(ui->safeSearchEdit->currentText().toStdString());	
			params.push_back("");
			params.push_back(ui->expiryEdit->itemData(ui->expiryEdit->currentIndex()).toString().toStdString());
			if(ui->multisigList->count() > 0)
			{
				params.push_back(QString::number(ui->multisigList->count()).toStdString());
				for(int i = 0; i < ui->multisigList->count(); ++i)
				{
					QString str = ui->multisigList->item(i)->text();
					arraySendParams.push_back(str.toStdString());
				}
				params.push_back(arraySendParams);
			}
			try {
				UniValue result = tableRPC.execute(strMethod, params);
				if (result.type() != UniValue::VNULL)
				{
				
					alias = ui->nameEdit->toPlainText() + ui->aliasEdit->text();
						
				}
				const UniValue& resArray = result.get_array();
				if(resArray.size() > 1)
				{
					const UniValue& complete_value = resArray[1];
					bool bComplete = false;
					if (complete_value.isStr())
						bComplete = complete_value.get_str() == "true";
					if(!bComplete)
					{
						string hex_str = resArray[0].get_str();
						GUIUtil::setClipboard(QString::fromStdString(hex_str));
						QMessageBox::critical(this, windowTitle(),
							tr("This transaction requires more signatures. Transaction hex <b>%1</b> has been copied to your clipboard for your reference. Please provide it to a signee that hasn't yet signed.").arg(QString::fromStdString(hex_str)),
								QMessageBox::Ok, QMessageBox::Ok);
						return false;
					}
				}

			}
			catch (UniValue& objError)
			{
				string strError = find_value(objError, "message").get_str();
				QMessageBox::critical(this, windowTitle(),
				tr("Error updating Alias: \"%1\"").arg(QString::fromStdString(strError)),
					QMessageBox::Ok, QMessageBox::Ok);
				break;
			}
			catch(std::exception& e)
			{
				QMessageBox::critical(this, windowTitle(),
					tr("General exception updating Alias"),
					QMessageBox::Ok, QMessageBox::Ok);
				break;
			}	
        }
        break;
    case TransferAlias:
        if(mapper->submit())
        {
			strMethod = string("aliasupdate");
			params.push_back(ui->aliasEdit->text().toStdString());
			params.push_back(ui->nameEdit->toPlainText().toStdString());
			params.push_back(ui->privateEdit->toPlainText().toStdString());
			params.push_back(ui->safeSearchEdit->currentText().toStdString());
			params.push_back(ui->transferEdit->text().toStdString());
			params.push_back(ui->expiryEdit->itemData(ui->expiryEdit->currentIndex()).toString().toStdString());
			try {
				UniValue result = tableRPC.execute(strMethod, params);
				if (result.type() != UniValue::VNULL)
				{

					alias = ui->nameEdit->toPlainText() + ui->aliasEdit->text()+ui->transferEdit->text();
						
				}
				const UniValue& resArray = result.get_array();
				if(resArray.size() > 1)
				{
					const UniValue& complete_value = resArray[1];
					bool bComplete = false;
					if (complete_value.isStr())
						bComplete = complete_value.get_str() == "true";
					if(!bComplete)
					{
						string hex_str = resArray[0].get_str();
						GUIUtil::setClipboard(QString::fromStdString(hex_str));
						QMessageBox::critical(this, windowTitle(),
							tr("This transaction requires more signatures. Transaction hex <b>%1</b> has been copied to your clipboard for your reference. Please provide it to a signee that hasn't yet signed.").arg(QString::fromStdString(hex_str)),
								QMessageBox::Ok, QMessageBox::Ok);
						return false;
					}
				}
			}
			catch (UniValue& objError)
			{
				string strError = find_value(objError, "message").get_str();
				QMessageBox::critical(this, windowTitle(),
                tr("Error transferring Alias: \"%1\"").arg(QString::fromStdString(strError)),
					QMessageBox::Ok, QMessageBox::Ok);
				break;
			}
			catch(std::exception& e)
			{
				QMessageBox::critical(this, windowTitle(),
                    tr("General exception transferring Alias"),
					QMessageBox::Ok, QMessageBox::Ok);
				break;
			}	
        }
        break;
    }
    return !alias.isEmpty();
}

void EditAliasDialog::accept()
{
    if(!model) return;

    if(!saveCurrentRow())
    {
        switch(model->getEditStatus())
        {
        case AliasTableModel::OK:
            // Failed with unknown reason. Just reject.
            break;
        case AliasTableModel::NO_CHANGES:
            // No changes were made during edit operation. Just reject.
            break;
        case AliasTableModel::INVALID_ALIAS:
            QMessageBox::warning(this, windowTitle(),
                tr("The entered alias \"%1\" is not a valid Syscoin Alias.").arg(ui->aliasEdit->text()),
                QMessageBox::Ok, QMessageBox::Ok);
            break;
        case AliasTableModel::DUPLICATE_ALIAS:
            QMessageBox::warning(this, windowTitle(),
                tr("The entered alias \"%1\" is already taken.").arg(ui->aliasEdit->text()),
                QMessageBox::Ok, QMessageBox::Ok);
            break;
        case AliasTableModel::WALLET_UNLOCK_FAILURE:
            QMessageBox::critical(this, windowTitle(),
                tr("Could not unlock wallet."),
                QMessageBox::Ok, QMessageBox::Ok);
            break;

        }
        return;
    }
    QDialog::accept();
}

QString EditAliasDialog::getAlias() const
{
    return alias;
}

void EditAliasDialog::setAlias(const QString &alias)
{
    this->alias = alias;
    ui->aliasEdit->setText(alias);
}
