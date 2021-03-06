#include "inmessagelistpage.h"
#include "ui_messagelistpage.h"
#include "guiutil.h"
#include "messagetablemodel.h"
#include "optionsmodel.h"
#include "walletmodel.h"
#include "platformstyle.h"
#include "newmessagedialog.h"
#include "messageinfodialog.h"
#include "csvmodelwriter.h"
#include "ui_interface.h"
#include <QSortFilterProxyModel>
#include <QClipboard>
#include <QMessageBox>
#include <QKeyEvent>
#include <QMenu>
using namespace std;

InMessageListPage::InMessageListPage(const PlatformStyle *platformStyle, QWidget *parent) :
    QDialog(parent),
    ui(new Ui::MessageListPage),
    model(0),
    optionsModel(0)
{
    ui->setupUi(this);
	QString theme = GUIUtil::getThemeName();  
	if (!platformStyle->getImagesOnButtons())
	{
		ui->exportButton->setIcon(QIcon());
		ui->newMessage->setIcon(QIcon());
		ui->detailButton->setIcon(QIcon());
		ui->copyMessage->setIcon(QIcon());
		ui->refreshButton->setIcon(QIcon());

	}
	else
	{
		ui->exportButton->setIcon(platformStyle->SingleColorIcon(":/icons/" + theme + "/export"));
		ui->newMessage->setIcon(platformStyle->SingleColorIcon(":/icons/" + theme + "/add"));
		ui->detailButton->setIcon(platformStyle->SingleColorIcon(":/icons/" + theme + "/details"));
		ui->copyMessage->setIcon(platformStyle->SingleColorIcon(":/icons/" + theme + "/editcopy"));
		ui->refreshButton->setIcon(platformStyle->SingleColorIcon(":/icons/" + theme + "/refresh"));
		
	}

    ui->labelExplanation->setText(tr("These are Syscoin messages sent to you."));
	
    // Context menu actions
    QAction *copyGuidAction = new QAction(ui->copyMessage->text(), this);
    QAction *copySubjectAction = new QAction(tr("Copy &Subject"), this);
	QAction *copyMessageAction = new QAction(tr("Copy &Msg"), this);
	QAction *newMessageAction = new QAction(tr("&New Msg"), this);
	QAction *replyMessageAction = new QAction(tr("&Reply Msg"), this);
	QAction *detailsAction = new QAction(tr("&Details"), this);
    // Build context menu
    contextMenu = new QMenu();
    contextMenu->addAction(copyGuidAction);
    contextMenu->addAction(copySubjectAction);
	contextMenu->addAction(copyMessageAction);
	contextMenu->addAction(detailsAction);
    contextMenu->addSeparator();
    contextMenu->addAction(newMessageAction);
	contextMenu->addAction(replyMessageAction);
    // Connect signals for context menu actions
    connect(copyGuidAction, SIGNAL(triggered()), this, SLOT(on_copyGuid_clicked()));
    connect(copySubjectAction, SIGNAL(triggered()), this, SLOT(on_copySubject_clicked()));
	connect(copyMessageAction, SIGNAL(triggered()), this, SLOT(on_copyMessage_clicked()));
	connect(newMessageAction, SIGNAL(triggered()), this, SLOT(on_newMessage_clicked()));
	connect(replyMessageAction, SIGNAL(triggered()), this, SLOT(on_replyMessage_clicked()));
	connect(detailsAction, SIGNAL(triggered()), this, SLOT(on_detailButton_clicked()));
   
	connect(ui->tableView, SIGNAL(doubleClicked(QModelIndex)), this, SLOT(on_detailButton_clicked()));
    connect(ui->tableView, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(contextualMenu(QPoint)));

}
void InMessageListPage::on_detailButton_clicked()
{
    if(!ui->tableView->selectionModel())
        return;
    QModelIndexList selection = ui->tableView->selectionModel()->selectedRows();
    if(!selection.isEmpty())
    {
        MessageInfoDialog dlg;
		QModelIndex origIndex = proxyModel->mapToSource(selection.at(0));
		dlg.setModel(walletModel, model);	
		dlg.loadRow(origIndex.row());
        dlg.exec();
    }
}
void InMessageListPage::on_newMessage_clicked()
{
    NewMessageDialog dlg(NewMessageDialog::NewMessage);
    dlg.setModel(walletModel, model);
    dlg.exec();
}
void InMessageListPage::on_replyMessage_clicked()
{
    if(!ui->tableView->selectionModel())
        return;
    QModelIndexList indexes = ui->tableView->selectionModel()->selectedRows();
    if(indexes.isEmpty())
        return;

    NewMessageDialog dlg(NewMessageDialog::ReplyMessage);
    dlg.setModel(walletModel, model);
    QModelIndex origIndex = proxyModel->mapToSource(indexes.at(0));
    dlg.loadRow(origIndex.row());
    dlg.exec();
}
InMessageListPage::~InMessageListPage()
{
    delete ui;
}
void InMessageListPage::on_refreshButton_clicked()
{
    if(!model)
        return;
    model->refreshMessageTable();
}
void InMessageListPage::showEvent ( QShowEvent * event )
{
    if(!walletModel) return;
}
void InMessageListPage::setModel(WalletModel* walletModel, MessageTableModel *model)
{
    this->model = model;
	this->walletModel = walletModel;
    if(!model) return;



    proxyModel = new QSortFilterProxyModel(this);
    proxyModel->setSourceModel(model);
    proxyModel->setDynamicSortFilter(true);
    proxyModel->setSortCaseSensitivity(Qt::CaseInsensitive);
    proxyModel->setFilterCaseSensitivity(Qt::CaseInsensitive);

	proxyModel->setFilterRole(MessageTableModel::TypeRole);

    ui->tableView->setModel(proxyModel);
    ui->tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->tableView->setSelectionMode(QAbstractItemView::SingleSelection);

    // Set column widths
    ui->tableView->setColumnWidth(0, 75); //guid
    ui->tableView->setColumnWidth(1, 75); //time
    ui->tableView->setColumnWidth(2, 100); //from
    ui->tableView->setColumnWidth(3, 100); //to
    ui->tableView->setColumnWidth(4, 300); //subject
    ui->tableView->setColumnWidth(5, 300); //message

    ui->tableView->horizontalHeader()->setStretchLastSection(true);

    connect(ui->tableView->selectionModel(), SIGNAL(selectionChanged(QItemSelection,QItemSelection)),
            this, SLOT(selectionChanged()));


    // Select row for newly created inmessage
    connect(model, SIGNAL(rowsInserted(QModelIndex,int,int)), this, SLOT(selectNewMessage(QModelIndex,int,int)));
    selectionChanged();

}

void InMessageListPage::setOptionsModel(OptionsModel *optionsModel)
{
    this->optionsModel = optionsModel;
}

void InMessageListPage::on_copyMessage_clicked()
{
   
    GUIUtil::copyEntryData(ui->tableView, MessageTableModel::Message);
}
void InMessageListPage::on_copyGuid_clicked()
{
   
    GUIUtil::copyEntryData(ui->tableView, MessageTableModel::GUID);
}
void InMessageListPage::on_copySubject_clicked()
{
   
    GUIUtil::copyEntryData(ui->tableView, MessageTableModel::Subject);
}



void InMessageListPage::selectionChanged()
{
    // Set button states based on selected tab and selection
    QTableView *table = ui->tableView;
    if(!table->selectionModel())
        return;

    if(table->selectionModel()->hasSelection())
    {
        ui->copyMessage->setEnabled(true);
		ui->detailButton->setEnabled(true);
    }
    else
    {
        ui->copyMessage->setEnabled(false);
		ui->detailButton->setEnabled(false);
    }
}
void InMessageListPage::keyPressEvent(QKeyEvent * event)
{
  if( event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter )
  {
	on_replyMessage_clicked();
    event->accept();
  }
  else
    QDialog::keyPressEvent( event );
}
void InMessageListPage::on_exportButton_clicked()
{
    // CSV is currently the only supported format
    QString filename = GUIUtil::getSaveFileName(
            this,
            tr("Export Message Data"), QString(),
            tr("Comma separated file (*.csv)"), NULL);

    if (filename.isNull()) return;

    CSVModelWriter writer(filename);

    // name, column, role
    writer.setModel(proxyModel);
    writer.addColumn("GUID", MessageTableModel::GUID, Qt::EditRole);
    writer.addColumn("Time", MessageTableModel::Time, Qt::EditRole);
    writer.addColumn("From", MessageTableModel::From, Qt::EditRole);
	writer.addColumn("To", MessageTableModel::To, Qt::EditRole);
	writer.addColumn("Subject", MessageTableModel::Subject, Qt::EditRole);
	writer.addColumn("Message", MessageTableModel::Message, Qt::EditRole);
    if(!writer.write())
    {
        QMessageBox::critical(this, tr("Error exporting"), tr("Could not write to file %1.").arg(filename),
                              QMessageBox::Abort, QMessageBox::Abort);
    }
}



void InMessageListPage::contextualMenu(const QPoint &point)
{
    QModelIndex index = ui->tableView->indexAt(point);
    if(index.isValid()) {
        contextMenu->exec(QCursor::pos());
    }
}

void InMessageListPage::selectNewMessage(const QModelIndex &parent, int begin, int /*end*/)
{
    QModelIndex idx = proxyModel->mapFromSource(model->index(begin, MessageTableModel::GUID, parent));
    if(idx.isValid() && (idx.data(Qt::EditRole).toString() == newMessageToSelect))
    {
        // Select row of newly created inmessage, once
        ui->tableView->setFocus();
        ui->tableView->selectRow(idx.row());
        newMessageToSelect.clear();
    }
}
