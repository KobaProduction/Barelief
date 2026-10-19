#include <core_app/resampler_comparison_dialog.h>
#include <visualization/ui_text.h>
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
static void build_comparison_inputs(ComparisonWidgets& w,QWidget* parent){
    w.dialog=new QDialog(parent);auto* dialog=w.dialog;dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setObjectName("resamplerComparisonDialog");
    dialog->setWindowTitle(ui::text("Resampler comparison"));dialog->resize(1000,460);
    w.layout=new QVBoxLayout(dialog);w.options=new QHBoxLayout;auto* layout=w.layout;auto* options=w.options;
    w.input=new QComboBox(dialog);auto* input=w.input;input->addItems({"48000","44100"});
    w.output=new QComboBox(dialog);auto* output=w.output;output->addItems({"32768","32000"});
    w.quality=new QSpinBox(dialog);auto* quality=w.quality;quality->setRange(0,10);quality->setValue(5);
    options->addWidget(new QLabel(ui::text("Input sample rate, Hz"),dialog));options->addWidget(input);
    options->addWidget(new QLabel(ui::text("Output sample rate, Hz"),dialog));options->addWidget(output);
    options->addWidget(new QLabel(ui::text("SpeexDSP quality"),dialog));options->addWidget(quality);
    w.run=new QPushButton(ui::text("Run comparison"),dialog);auto* run=w.run;run->setObjectName("runResamplerComparison");options->addWidget(run);layout->addLayout(options);
}
static void build_comparison_table(ComparisonWidgets& w){
    auto* dialog=w.dialog;auto* layout=w.layout;
    auto* note=new QLabel(ui::text("Silent synthetic input; processing time per 480 input samples. RAM excludes allocator and driver buffers. IIR is experimental; linear downsampling risks aliasing."),dialog);note->setWordWrap(true);layout->addWidget(note);
    w.table=new QTableWidget(int(signal_contracts::resampler_policies().size()),7,dialog);auto* table=w.table;table->setObjectName("resamplerComparisonResults");
    table->setHorizontalHeaderLabels({ui::text("Resampling method"),"Fs out",ui::text("Prepare, us"),"p50, us","p95, us",ui::text("Owned payload, bytes"),ui::text("Content delay, input samples")});
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);table->setEditTriggers(QAbstractItemView::NoEditTriggers);layout->addWidget(table);
}
static void bind_comparison_worker(const ComparisonWidgets& w,ResamplerComparisonService compare){
    auto* dialog=w.dialog;auto* run=w.run;auto* input=w.input;auto* output=w.output;auto* quality=w.quality;auto* table=w.table;
    auto stop=std::make_shared<std::atomic_bool>(false);
    QObject::connect(dialog,&QObject::destroyed,[stop]{stop->store(true);});
    QObject::connect(run,&QPushButton::clicked,dialog,[=]{
        run->setEnabled(false);const auto in=input->currentText().toUInt(),out=output->currentText().toUInt();const auto q=unsigned(quality->value());
        const QPointer<QDialog> guard(dialog);
        auto* thread=QThread::create([=]{
            const auto results=compare?compare({in,out,q},*stop):std::vector<desktop_analysis::offline::ComparisonResult>{};
            if(stop->load())return;
            // Queue against the application, whose lifetime encloses this joined
            // worker. The dialog can be destroyed while the measurement runs.
            QMetaObject::invokeMethod(QCoreApplication::instance(),[=]{
                if(!guard)return;
                for(const auto& r:results){
                    QStringList values{ui::text(presentation::resampler_caption(r.mode)),r.valid?QString::number(r.rate):ui::text("Unavailable"),QString::number(r.prepare,'f',1),QString::number(r.p50,'f',1),QString::number(r.p95,'f',1),QString::number(r.resources.owned_bytes),QString::number(r.resources.fixed_delay_input_samples)+(r.resources.frequency_dependent_delay?QStringLiteral(" + ")+ui::text("frequency dependent"):QString{})};
                    for(int c=0;c<values.size();++c)table->setItem(int(r.mode),c,new QTableWidgetItem(values[c]));
                }run->setEnabled(true);
            },Qt::QueuedConnection);
        });
        QObject::connect(dialog,&QObject::destroyed,thread,[stop,thread]{stop->store(true);thread->wait();},Qt::DirectConnection);
        QObject::connect(thread,&QThread::finished,thread,&QObject::deleteLater);thread->start();
    });
}
void show_resampler_comparison(QWidget* parent,ResamplerComparisonService compare){
    ComparisonWidgets widgets;build_comparison_inputs(widgets,parent);build_comparison_table(widgets);bind_comparison_worker(widgets,std::move(compare));widgets.dialog->show();
}
