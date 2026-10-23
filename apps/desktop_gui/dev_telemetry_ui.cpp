#include "dev_telemetry_ui.h"
#include <core/telemetry.h>
#include <core/performance.h>
#include <core_app/main_window.h>
#include <QApplication>
#include <QMenuBar>
#include <QProcess>
#include <QFileInfo>
#include <QMessageBox>
#include <QTimer>
#include <QDialog>
#include <QVBoxLayout>
#include <QLabel>
#include <QTableWidget>
#include <QHeaderView>

namespace {
void refresh_statistics(QTableWidget& table) {
    table.setRowCount(0);
    for(size_t i=0;i<core::perf::counters.size();++i) {
        const auto& counter=core::perf::counters[i];const auto count=counter.count.load();
        if(!count)continue;
        const auto row=table.rowCount();table.insertRow(row);
        const QStringList values{QString::fromUtf8(core::perf::names[i]),QString::number(count),
            QString::number(double(counter.total.load())/double(count)/1e6,'f',3),
            QString::number(double(counter.maximum.load())/1e6,'f',3),
            QString::number(double(core::perf::percentile_upper_ns(counter,95))/1e6,'f',3)};
        for(int column=0;column<values.size();++column)table.setItem(row,column,new QTableWidgetItem(values[column]));
    }
}
QDialog* create_statistics(QWidget* parent) {
    auto* dialog=new QDialog(parent);dialog->setWindowTitle(QStringLiteral("Developer telemetry"));
    dialog->setObjectName("devTelemetryStatistics");dialog->resize(850,520);
    auto* layout=new QVBoxLayout(dialog);
    layout->addWidget(new QLabel(QStringLiteral("Since application start. Nested stages overlap. Full timeline: Open Tracy."),dialog));
    auto* table=new QTableWidget(dialog);table->setColumnCount(5);
    table->setHorizontalHeaderLabels({QStringLiteral("Stage"),QStringLiteral("Calls"),QStringLiteral("Mean, ms"),QStringLiteral("Max, ms"),QStringLiteral("p95 upper, ms")});
    table->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);layout->addWidget(table);
    auto* timer=new QTimer(dialog);timer->setInterval(500);
    QObject::connect(timer,&QTimer::timeout,dialog,[dialog,table]{if(dialog->isVisible())refresh_statistics(*table);});
    timer->start();return dialog;
}
void open_profiler(QWidget* parent) {
    auto executable=qEnvironmentVariable("BARELIEF_TRACY_PROFILER");
    if(executable.isEmpty())executable=QStringLiteral(BARELIEF_TRACY_PROFILER_PATH);
    if(QFileInfo::exists(executable) && QProcess::startDetached(executable,{}))return;
    QMessageBox::information(parent,QStringLiteral("Tracy"),QStringLiteral(
        "Run scripts/tracy.ps1 -Action Setup, or set BARELIEF_TRACY_PROFILER to tracy-profiler.exe.\n"
        "Connect to 127.0.0.1:8086. The viewer must match Tracy 0.14.1."));
}
void add_menu(MainWindow& window) {
    auto* menu=window.menuBar()->addMenu(QStringLiteral("Telemetry (dev)"));
    menu->setObjectName("devTelemetryMenu");
    auto* toggle=menu->addAction(QStringLiteral("Enable collection"));toggle->setCheckable(true);
    toggle->setChecked(core::telemetry::enabled());toggle->setObjectName("devTelemetryEnabled");
    QObject::connect(toggle,&QAction::toggled,menu,[](bool enabled){core::telemetry::set_enabled(enabled);});
    auto* statistics=create_statistics(&window);
    QObject::connect(menu->addAction(QStringLiteral("Live statistics")),&QAction::triggered,statistics,&QDialog::show);
    QObject::connect(menu->addAction(QStringLiteral("Open Tracy")),&QAction::triggered,menu,[&window]{open_profiler(&window);});
    auto* status=menu->addAction(QString{});status->setEnabled(false);
    QObject::connect(menu,&QMenu::aboutToShow,menu,[status,toggle]{
        toggle->setChecked(core::telemetry::enabled());
        status->setText(core::telemetry::active()?QStringLiteral("Connected; collection active"):
            core::telemetry::enabled()?QStringLiteral("Waiting for connection on localhost:8086"):QStringLiteral("Collection disabled"));
    });
}
}
void install_dev_telemetry_ui() {
    for(const auto widgets=QApplication::topLevelWidgets();auto* widget:widgets)
        if(auto* window=qobject_cast<MainWindow*>(widget)){add_menu(*window);return;}
}
