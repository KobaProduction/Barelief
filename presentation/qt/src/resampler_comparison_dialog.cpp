#include <core_app/resampler_comparison_dialog.h>
#include <barelief/ui_text.h>
#include <offline/resampler_comparison.h>
#include <core_app/resampler_presentation.h>
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QSpinBox>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QHeaderView>
#include <QThread>
#include <QPointer>
#include <QCoreApplication>
#include <atomic>
#include <chrono>
#include <cmath>
#include <memory>
#include <algorithm>

struct ComparisonWidgets {
    QDialog* dialog;
    QVBoxLayout* layout;
    QHBoxLayout* options;
    QComboBox *input,*output;
    QSpinBox* quality;
    QPushButton* run;
    QTableWidget* table;
};
static QComboBox* comparison_rates(QDialog* dialog,const QStringList& rates){
    auto* input=new QComboBox(dialog);input->addItems(rates);return input;
}
static void append_comparison_option(ComparisonWidgets& w,const char* label,QWidget* control){
    w.options->addWidget(new QLabel(ui::text(label),w.dialog));w.options->addWidget(control);
}
static void build_comparison_options(ComparisonWidgets& w){
    w.input=comparison_rates(w.dialog,{"48000","44100"});
    w.output=comparison_rates(w.dialog,{"32768","32000"});
    w.quality=new QSpinBox(w.dialog);w.quality->setRange(0,10);w.quality->setValue(5);
    append_comparison_option(w,"Input sample rate, Hz",w.input);
    append_comparison_option(w,"Output sample rate, Hz",w.output);
    append_comparison_option(w,"SpeexDSP quality",w.quality);
    w.run=new QPushButton(ui::text("Run comparison"),w.dialog);w.run->setObjectName("runResamplerComparison");
    w.options->addWidget(w.run);w.layout->addLayout(w.options);
}
static void build_comparison_inputs(ComparisonWidgets& w,QWidget* parent){
    w.dialog=new QDialog(parent);auto* dialog=w.dialog;dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setObjectName("resamplerComparisonDialog");
    dialog->setWindowTitle(ui::text("Resampler comparison"));dialog->resize(1000,460);
    w.layout=new QVBoxLayout(dialog);w.options=new QHBoxLayout;
    build_comparison_options(w);
}
static void build_comparison_table(ComparisonWidgets& w){
    auto* dialog=w.dialog;auto* layout=w.layout;
    auto* note=new QLabel(ui::text("Silent synthetic input; processing time per 480 input samples. RAM excludes allocator and driver buffers. IIR is experimental; linear downsampling risks aliasing."),dialog);note->setWordWrap(true);layout->addWidget(note);
    w.table=new QTableWidget(int(signal_contracts::resampler_policies().size()),7,dialog);auto* table=w.table;table->setObjectName("resamplerComparisonResults");
    table->setHorizontalHeaderLabels({ui::text("Resampling method"),"Fs out",ui::text("Prepare, us"),"p50, us","p95, us",ui::text("Owned payload, bytes"),ui::text("Content delay, input samples")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);table->setEditTriggers(QAbstractItemView::NoEditTriggers);layout->addWidget(table);
}
static QStringList comparison_values(const desktop_analysis::offline::ComparisonResult& r){
    return {ui::text(presentation::resampler_caption(r.mode)),r.valid?QString::number(r.rate):ui::text("Unavailable"),
        QString::number(r.prepare,'f',1),QString::number(r.p50,'f',1),QString::number(r.p95,'f',1),
        QString::number(r.resources.owned_bytes),QString::number(r.resources.fixed_delay_input_samples)+
        (r.resources.frequency_dependent_delay?QStringLiteral(" + ")+ui::text("frequency dependent"):QString{})};
}
static void publish_comparison(const ComparisonWidgets& w,const std::vector<desktop_analysis::offline::ComparisonResult>& results){
    for(const auto& r:results){
        const auto values=comparison_values(r);
        for(int c=0;c<values.size();++c)w.table->setItem(int(r.mode),c,new QTableWidgetItem(values[c]));
    }
    w.run->setEnabled(true);
}
static void start_comparison(const ComparisonWidgets& w,ResamplerComparisonService compare,std::shared_ptr<std::atomic_bool> stop){
    w.run->setEnabled(false);
    const auto in=w.input->currentText().toUInt(),out=w.output->currentText().toUInt();
    const auto quality=unsigned(w.quality->value());
    const QPointer<QDialog> guard(w.dialog);
    auto* thread=QThread::create([=]{
        const auto results=compare?compare({in,out,quality},*stop):std::vector<desktop_analysis::offline::ComparisonResult>{};
        if(stop->load())return;
        // The application outlives the joined worker; dialog lifetime is checked on delivery.
        QMetaObject::invokeMethod(QCoreApplication::instance(),[=]{
            if(guard)publish_comparison(w,results);
        },Qt::QueuedConnection);
    });
    QObject::connect(w.dialog,&QObject::destroyed,thread,[stop,thread]{stop->store(true);thread->wait();},Qt::DirectConnection);
    QObject::connect(thread,&QThread::finished,thread,&QObject::deleteLater);thread->start();
}
static void bind_comparison_worker(const ComparisonWidgets& w,ResamplerComparisonService compare){
    auto stop=std::make_shared<std::atomic_bool>(false);
    QObject::connect(w.dialog,&QObject::destroyed,[stop]{stop->store(true);});
    QObject::connect(w.run,&QPushButton::clicked,w.dialog,[=]{start_comparison(w,compare,stop);});
}
void show_resampler_comparison(QWidget* parent,ResamplerComparisonService compare){
    ComparisonWidgets widgets;build_comparison_inputs(widgets,parent);build_comparison_table(widgets);bind_comparison_worker(widgets,std::move(compare));widgets.dialog->show();
}
