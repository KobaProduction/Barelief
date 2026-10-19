#include "product_information.h"
#include "product_updates.h"
#include <core_app/product_identity.h>
#include <barelief/ui_text.h>
#include <QApplication>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QComboBox>
#include <QDirIterator>
#include <QFile>
#include <QLabel>
#include <QMainWindow>
#include <QMenuBar>
#include <QPushButton>
#include <QTextBrowser>
#include <QVBoxLayout>

namespace {
void select_license(QComboBox& choices,QTextBrowser& text) {
    QFile file(choices.currentData().toString());if(!file.open(QIODevice::ReadOnly))return;
    text.setPlainText(QString::fromUtf8(file.readAll()));
}
void populate_licenses(QComboBox& choices) {
    choices.addItem(QStringLiteral("Barelief — MIT"),QStringLiteral(":/barelief/LICENSE"));
    choices.addItem(ui::text("Third-party licenses"),QStringLiteral(":/barelief/THIRD_PARTY_NOTICES.md"));
    const QString prefix=QStringLiteral(":/barelief/packaging/licenses/");
    QDirIterator files(prefix,QDir::Files,QDirIterator::Subdirectories);
    while(files.hasNext()){
        const auto path=files.next();choices.addItem(path.mid(prefix.size()),path);
    }
}
void show_licenses(QWidget* parent) {
    auto* dialog=new QDialog(parent);dialog->setAttribute(Qt::WA_DeleteOnClose);
    ui::bind(dialog,"windowTitle","Licenses");dialog->resize(740,540);
    auto* layout=new QVBoxLayout(dialog);auto* choices=new QComboBox(dialog);layout->addWidget(choices);
    auto* text=new QTextBrowser(dialog);layout->addWidget(text);populate_licenses(*choices);select_license(*choices,*text);
    QObject::connect(choices,&QComboBox::currentIndexChanged,text,[choices,text]{select_license(*choices,*text);});
    dialog->show();
}
void add_update_controls(QVBoxLayout& layout) {
    auto* status=new QLabel;ui::bind(status,"text","Check GitHub for a newer stable release.");status->setWordWrap(true);
    auto* check=new QPushButton;ui::bind(check,"text","Check for updates");check->setObjectName("checkUpdatesButton");
    auto* release=new QPushButton;ui::bind(release,"text","View releases");release->setVisible(false);
    layout.addWidget(status);layout.addWidget(check);layout.addWidget(release);
    QObject::connect(check,&QPushButton::clicked,check,[check,status,release]{barelief::check_updates(*check,*status,*release);});
    QObject::connect(release,&QPushButton::clicked,release,[]{QDesktopServices::openUrl(QUrl(barelief::releases));});
}
void add_identity(QVBoxLayout& layout) {
    auto* logo=new QLabel;logo->setPixmap(qApp->windowIcon().pixmap(96,96));layout.addWidget(logo,0,Qt::AlignHCenter);
    auto* title=new QLabel(QStringLiteral("Barelief %1").arg(barelief::version));layout.addWidget(title);
    auto* description=new QLabel;ui::bind(description,"text","Signal Processing & Visualization");layout.addWidget(description);
    auto* publisher=new QLabel;ui::bind(publisher,"text","Developer and publisher: Koba Production");layout.addWidget(publisher);
    auto* repository=new QPushButton;ui::bind(repository,"text","GitHub repository");layout.addWidget(repository);
    QObject::connect(repository,&QPushButton::clicked,repository,[]{QDesktopServices::openUrl(QUrl(barelief::repository));});
}
void add_about_content(QDialog& dialog) {
    auto* layout=new QVBoxLayout(&dialog);add_identity(*layout);
    auto* licenses=new QPushButton;ui::bind(licenses,"text","Licenses");layout->addWidget(licenses);
    QObject::connect(licenses,&QPushButton::clicked,licenses,[&dialog]{show_licenses(&dialog);});
    add_update_controls(*layout);
    auto* buttons=new QDialogButtonBox(QDialogButtonBox::Close);layout->addWidget(buttons);
    ui::bind(buttons->button(QDialogButtonBox::Close),"text","Close");
    QObject::connect(buttons,&QDialogButtonBox::rejected,&dialog,&QDialog::close);
}
}
void barelief::install_product_information(QMainWindow& window) {
    auto* dialog=new QDialog(&window);dialog->setObjectName("bareliefAboutDialog");
    ui::bind(dialog,"windowTitle","About Barelief");dialog->resize(460,480);add_about_content(*dialog);
    auto* help=window.menuBar()->addMenu(QString{});ui::bind(help,"title","Help");
    auto* about=help->addAction(QString{});ui::bind(about,"text","About Barelief");about->setObjectName("aboutBareliefAction");
    QObject::connect(about,&QAction::triggered,dialog,[dialog]{dialog->show();});
}
