#include "oscillator_bank_widget.h"
#include "literal_edit_binding.h"
#include "core_app/scheme_editor/node_parameter_model.h"
#include "visualization/ui_text.h"
#include <QCheckBox>
#include <QComboBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <QPointer>

OscillatorBankWidget::OscillatorBankWidget(NodeParameterModel* source,QWidget* parent)
    : QTableWidget(std::get<int>(source->field("oscillatorCount")->read()),8,parent) {
    const QPointer<NodeParameterModel> model(source);
    setObjectName("oscillatorTable");
    const auto headers=[this]{
        setHorizontalHeaderLabels({"ID",ui::text("Enabled"),ui::text("Frequency, Hz"),ui::text("Amplitude"),
            ui::text("Phase, radians"),ui::text("Attack, seconds"),ui::text("Release, seconds"),ui::text("Remove oscillator")});
    };
    headers();connect(&ui::Language::instance(),&ui::Language::changed,this,headers);
    horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    setMinimumWidth(0);setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);setMaximumHeight(310);
    const QStringList suffixes{"frequency_hz","amplitude","phase_radians","attack_seconds","release_seconds"};
    for(int row=0;row<rowCount();++row)setupRow(row,model,suffixes);
    resizeRowsToContents();
    connect(model.data(),&NodeParameterModel::editStateChanged,this,[this]{resizeRowsToContents();});
}
void OscillatorBankWidget::setupRow(int row,QPointer<NodeParameterModel> model,const QStringList& suffixes) {
    const auto prefix=QString("oscillator%1").arg(row);
    setupRowControls(row,model,prefix);
    for(int column=0;column<suffixes.size();++column)
        setupValueCell(row,column,model,prefix+suffixes[column]);
}
void OscillatorBankWidget::setupRowControls(int row,QPointer<NodeParameterModel> model,const QString& prefix) {
        auto* id=new QLabel(this);id->setObjectName(prefix+"Id");
        setCellWidget(row,0,id);
        auto* enabled=new QCheckBox(this);enabled->setObjectName(prefix+"Enabled");setCellWidget(row,1,enabled);
        const auto booleanKey=prefix+"Enabled";
        connect(enabled,&QCheckBox::toggled,enabled,[model,booleanKey](bool value){if(model)model->edit(booleanKey,value);});
        auto* remove=new QPushButton(this);remove->setObjectName(prefix+"Remove");
        ui::bind(remove,"text","Remove oscillator");setCellWidget(row,7,remove);
        connect(remove,&QPushButton::clicked,remove,[model,prefix]{if(model)model->edit(prefix+"Remove",false);});
        const auto refreshRow=[model,prefix,id,enabled,remove]{
            if(!model)return;
            if(const auto* f=model->field(prefix+"Id"))id->setText(std::get<QString>(f->read()));
            if(const auto* f=model->field(prefix+"Enabled")){const QSignalBlocker block(enabled);enabled->setChecked(std::get<bool>(f->read()));}
            const auto* f=model->field(prefix+"Remove");remove->setEnabled(f && (!f->available || f->available()));
        };
        connect(model.data(),&NodeParameterModel::valuesChanged,this,refreshRow);refreshRow();
}
void OscillatorBankWidget::setupValueCell(int row,int column,QPointer<NodeParameterModel> model,const QString& key) {
            auto* box=new QWidget(this);auto* layout=new QVBoxLayout(box);layout->setContentsMargins(2,2,2,2);
            auto* edit=new QLineEdit(box);edit->setObjectName(key);
            auto* type=new QComboBox(box);type->addItems({"int64","uint64","float32","float64"});type->setCurrentIndex(3);type->hide();
            auto* error=new QLabel(box);error->setObjectName(key+"_error");error->setWordWrap(true);
            layout->addWidget(edit);layout->addWidget(error);setCellWidget(row,column+2,box);
            const auto refreshInput=numeric_editor::bind_literal_input(model.data(),key,type,edit);
            const auto refresh=[model,key,edit,error,refreshInput]{
                if(!model)return;refreshInput();const auto* f=model->field(key);edit->setEnabled(f && (!f->available || f->available()));
                const auto state=model->editState(key);auto message=ui::text(state.error.toUtf8().constData());
                for(const auto& argument:state.error_arguments)message=message.arg(argument);
                error->setText(message);error->setVisible(!message.isEmpty());edit->setToolTip(message);
            };
            connect(model.data(),&NodeParameterModel::valuesChanged,box,refresh);
            connect(model.data(),&NodeParameterModel::editStateChanged,box,[key,refresh](const QString& changed){if(changed==key)refresh();});
            connect(&ui::Language::instance(),&ui::Language::changed,box,refresh);refresh();
}
