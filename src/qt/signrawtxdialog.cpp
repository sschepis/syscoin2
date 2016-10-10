#include "signrawtxdialog.h"
#include "ui_signrawtxdialog.h"

#include "guiutil.h"
#include "walletmodel.h"
#include "syscoingui.h"
#include "ui_interface.h"
#include <QMessageBox>
#include <QJsonDocument>
#include "rpc/server.h"
#include <QSettings>
using namespace std;

extern CRPCTable tableRPC;
SignRawTxDialog::SignRawTxDialog(QWidget* parent) :
	QDialog(parent),
    ui(new Ui::SignRawTxDialog)
{
	ui->setupUi(this);
	ui->rawTxDisclaimer->setText(tr("<font color='blue'>Sign a raw syscoin transaction and send it to the network if it is complete with all required signatures. Enter the raw hex encoded transaction below.</font>")); 
	ui->decodeTxDisclaimer->setText(tr("<font color='blue'>Once you enter a valid raw transaction in the general section this area will become populated with the raw transaction information including any syscoin related service information so you will know what the transaction is doing before signing and potentially sending it to the network.</font>"));
	ui->decodeSysTxDisclaimer->setText(tr("<font color='blue'>The area below is to display syscoin specific information regarding this transaction. Currently there is nothing to display.</font>")); 
	connect(ui->rawTxEdit, SIGNAL(textChanged()), this, SLOT(rawTxChanged()));
}
void SignRawTxDialog::setRawTxEdit()
{
	UniValue params(UniValue::VARR);
	UniValue arraySendParams(UniValue::VARR);
	string strMethod;
	strMethod = string("decoderawtransaction");
	params.push_back(ui->rawTxEdit->toPlainText().toStdString());

	try {
        UniValue result = tableRPC.execute(strMethod, params);
		QString jsonString = QString::fromStdString(result.write());
		QJsonDocument doc = QJsonDocument::fromJson(jsonString.toUtf8());
		QString formattedJsonString = doc.toJson(QJsonDocument::Indented);

		ui->rawSysTxDecodeEdit->setPlainText(formattedJsonString);

	}
	catch (UniValue& objError)
	{
		string strError = find_value(objError, "message").get_str();
		ui->rawTxDecodeEdit->setText(tr("Error creating decoding raw transaction: \"%1\"").arg(QString::fromStdString(strError)));
	}
	catch(std::exception& e)
	{
		ui->rawTxDecodeEdit->setText(tr("General exception decoding raw transaction"));
	}	
}
void SignRawTxDialog::setRawSysTxEdit()
{
	UniValue params(UniValue::VARR);
	UniValue arraySendParams(UniValue::VARR);
	string strMethod;
	strMethod = string("decodesysrawtransaction");
	params.push_back(ui->rawTxEdit->toPlainText().toStdString());

	try {
        UniValue result = tableRPC.execute(strMethod, params);
		QString jsonString = QString::fromStdString(result.write());
		QJsonDocument doc = QJsonDocument::fromJson(jsonString.toUtf8());
		QString formattedJsonString = doc.toJson(QJsonDocument::Indented);

		ui->rawSysTxDecodeEdit->setPlainText(formattedJsonString);
	}
	catch (UniValue& objError)
	{
		string strError = find_value(objError, "message").get_str();
		ui->rawTxDecodeEdit->setText(tr("Error creating decoding raw transaction: \"%1\"").arg(QString::fromStdString(strError)));
	}
	catch(std::exception& e)
	{
		ui->rawTxDecodeEdit->setText(tr("General exception decoding raw transaction"));
	}	
}
void SignRawTxDialog::rawTxChanged()
{
	setRawTxEdit();
	//setRawSysTxEdit();
}
SignRawTxDialog::~SignRawTxDialog()
{
    delete ui;
}

bool SignRawTxDialog::saveCurrentRow()
{

	UniValue params(UniValue::VARR);
	UniValue arraySendParams(UniValue::VARR);
	string strMethod;
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
			QMessageBox::information(this, windowTitle(),
				tr("This transaction requires more signatures. Transaction hex <b>%1</b> has been copied to your clipboard for your reference. Please provide it to a signee that hasn't yet signed.").arg(QString::fromStdString(hex_str)),
					QMessageBox::Ok, QMessageBox::Ok);
		}
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
	return true;
}

void SignRawTxDialog::accept()
{
	bool saveState = saveCurrentRow();
    if(saveState)
		QDialog::accept();
}

