#include "core_app/scheme_editor/node_parameter_form.h"
#include "core_app/ui/field_row.h"
#include "core_app/ui/collapsible_section.h"
#include "core_app/wheel_input.h"
#include "barelief/ui_text.h"
#include "literal_edit_binding.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QComboBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QSpinBox>
#include <QPushButton>
#include <QToolButton>
#include <QLabel>
#include <QAction>
#include <QTimer>
#include <QSignalBlocker>
#include <QVariant>
#include <QElapsedTimer>
#include <QDebug>
#include <type_traits>
#include <algorithm>

namespace {
QString fieldCaption(const NodeParameterModel::Field& field) {
    auto text=ui::text(field.label.toUtf8().constData());
    for(const auto& argument:field.caption_arguments)text=text.arg(argument);
    return text;
}
QString optionLabel(const NodeParameterModel::Option& option) {
    auto label=option.translate?ui::text(option.label.toUtf8().constData()):option.label;
    for(const auto& argument:option.label_arguments)label=label.arg(argument);
    return label;
}
QVariant optionData(const NodeParameterModel::Value& value) {
    return std::visit([](const auto& item)->QVariant {
        using T=std::decay_t<decltype(item)>;
        if constexpr(std::is_same_v<T,signal_contracts::NumericLiteral>)return {};
        else return QVariant::fromValue(item);
    },value);
}
void populateChoices(QComboBox* combo,const NodeParameterModel::Field& field,const NodeParameterModel::Value& value) {
    combo->clear();
    int selected=-1;
    for(std::size_t i=0;i<field.options.size();++i){
        const auto& option=field.options[i];
        combo->addItem(optionLabel(option),optionData(option.value));
        if(option.value==value)selected=int(i);
    }
    combo->setCurrentIndex(selected);
}
QComboBox* literalTypeEditor(QWidget* parent,const NodeParameterModel::Field& field) {
    auto* type=new QComboBox(parent);type->addItems({"int64","uint64","float32","float64"});
    type->setObjectName(field.key+"_type");
    if(field.literal_type){type->setCurrentIndex(*field.literal_type);type->hide();}
    return type;
}
QString fieldError(const NodeParameterModel::Field* field,const NodeParameterModel::EditState& state) {
    auto message=ui::text(state.error.toUtf8().constData());
    for(const auto& argument:state.error_arguments)message=message.arg(argument);
    if(!message.isEmpty() || !field || field->editor!=NodeParameterModel::Editor::Integer || !field->read)return message;
    const auto value=std::get<int>(field->read());
    if(value<field->minimum || value>field->maximum)return ui::text(field->bounds_error.toUtf8().constData());
    if(field->available && !field->available())return ui::text(field->availability_error.toUtf8().constData());
    return {};
}
}
NodeParameterForm::NodeParameterForm(NodeParameterModel* model,bool inline_form,QWidget* parent,bool contracts_only,CompositeFactory composite_factory)
    :QWidget(parent),_model(model),_inline_form(inline_form),_contracts_only(contracts_only),_composite_factory(std::move(composite_factory)) {
    desktop_input::protect_parameter_wheels();setObjectName("numericNodeParameters");
    setProperty("scrollSafeControls",true);setProperty("inlineForm",inline_form);
    setSizePolicy(inline_form?QSizePolicy::Preferred:QSizePolicy::Ignored,QSizePolicy::Preferred);
    connect(model,&NodeParameterModel::structureChanged,this,[this]{scheduleRebuild();});
    connect(model,&NodeParameterModel::presentationChanged,this,[this]{ensurePinnedFields();});
    connect(&ui::Language::instance(),&ui::Language::changed,this,[this]{if(_model)emit _model->valuesChanged();});
    rebuild();
}
void NodeParameterForm::scheduleRebuild() {
    if(_rebuild_pending)return;
    _rebuild_pending=true;
    QTimer::singleShot(0,this,[this]{_rebuild_pending=false;rebuild();});
}
void NodeParameterForm::clearRows() {
    for(const auto& connection:_bindings)disconnect(connection);
    _bindings.clear();
    _materialized_fields.clear();
    const auto children=findChildren<QWidget*>(QString{},Qt::FindDirectChildrenOnly);
    for(auto* child:children)delete child;
    delete layout();
}
void NodeParameterForm::addBypass(QVBoxLayout* root) {
    if(_inline_form || _contracts_only || !_model->supportsBypass())return;
    auto* bypass=new QCheckBox(this);bypass->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
    bypass->setObjectName("numericBypass");ui::bind(bypass,"text","Bypass");
    ui::bind(bypass,"toolTip","Pass the input through without processing.");
    bypass->setChecked(_model->bypassed());root->addWidget(bypass,0,Qt::AlignRight);
    bypass->setVisible(_model->supportsBypass());
    bindBypass(bypass);
}
void NodeParameterForm::bindBypass(QCheckBox* bypass) {
    _bindings.push_back(connect(_model,&NodeParameterModel::valuesChanged,bypass,[this,bypass]{
        if(!_model)return;
        QSignalBlocker block(bypass);bypass->setChecked(_model->bypassed());
    }));
    connect(bypass,&QCheckBox::toggled,bypass,[this](bool checked){if(_model)_model->setBypassed(checked);});
}
ui::CollapsibleSection* NodeParameterForm::createPlacement() {
    if(_inline_form || _contracts_only)return nullptr;
    auto* placement=new ui::CollapsibleSection("Fields on block",this);
    placement->setObjectName("fieldsOnBlockSection");placement->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    if(auto* toggle=placement->findChild<QToolButton*>())toggle->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
    placement->setExpanded(_placement_expanded);
    connect(placement,&ui::CollapsibleSection::expandedChanged,this,[this](bool expanded){_placement_expanded=expanded;});
    return placement;
}
bool NodeParameterForm::showsField(const Field& field) const {
    if((field.section==NodeParameterModel::Section::Contract)!=_contracts_only)return false;
    if(!_inline_form && field.hide_inspector)return false;
    return !_inline_form || (!field.hide_inline && (_materialized_fields.contains(field.key) || _model->onBlock(field.key)));
}
void NodeParameterForm::ensurePinnedFields() {
    if(!_inline_form || !_model)return;
    for(const auto& field:_model->fields())
        if(showsField(field) && !_materialized_fields.contains(field.key)){scheduleRebuild();return;}
}
NodeParameterForm::ControlBinding NodeParameterForm::literalControl(const Field& field) {
    auto* box=new QWidget(this);auto* horizontal=new QHBoxLayout(box);
    horizontal->setContentsMargins(0,0,0,0);
    if(!_inline_form)horizontal->setDirection(QBoxLayout::TopToBottom);
    auto* edit=new QLineEdit(box);
    edit->setObjectName(field.key);box->setFocusProxy(edit);
    box->setObjectName(field.key+"_field");
    if(field.literal_type){horizontal->addWidget(edit);return {box,numeric_editor::bind_literal_input(_model,field.key,*field.literal_type,edit)};}
    auto* type=literalTypeEditor(box,field);horizontal->addWidget(type);horizontal->addWidget(edit);
    return {box,numeric_editor::bind_literal_input(_model,field.key,type,edit)};
}
void NodeParameterForm::refreshChoices(const QString& key,QComboBox* combo) {
    if(!_model)return;
    const auto* field=_model->field(key);if(!field || !field->read)return;
    const auto value=field->read();
    QSignalBlocker blocker(combo);
    populateChoices(combo,*field,value);
}
void NodeParameterForm::editChoice(const QString& key,int index) {
    if(!_model || index<0)return;
    const auto* field=_model->field(key);if(!field || index>=int(field->options.size()))return;
    const auto value=field->options[std::size_t(index)].value;_model->edit(key,value);
}
NodeParameterForm::ControlBinding NodeParameterForm::choiceControl(const Field& field) {
    auto* combo=new QComboBox(this);
    connect(combo,&QComboBox::currentIndexChanged,combo,[this,key=field.key](int index){editChoice(key,index);});
    return {combo,[this,key=field.key,combo]{refreshChoices(key,combo);}};
}
NodeParameterForm::ControlBinding NodeParameterForm::booleanControl(const Field& field) {
    auto* check=new QCheckBox(this);
    auto refresh=[this,key=field.key,check]{
        if(!_model)return;
        const auto* current=_model->field(key);if(!current || !current->read)return;
        QSignalBlocker block(check);check->setChecked(std::get<bool>(current->read()));
    };
    connect(check,&QCheckBox::toggled,check,[this,key=field.key](bool value){if(_model)_model->edit(key,value);});
    return {check,refresh};
}
NodeParameterForm::ControlBinding NodeParameterForm::integerControl(const Field& field) {
    auto* spin=new QSpinBox(this);
    auto refresh=[this,key=field.key,spin]{
        if(!_model)return;
        const auto* current=_model->field(key);if(!current || !current->read)return;
        const auto value=std::get<int>(current->read());
        QSignalBlocker block(spin);spin->setRange(std::min(current->minimum,value),std::max(current->maximum,value));spin->setValue(value);
        spin->setSpecialValueText(ui::text(current->special_value_text.toUtf8().constData()));
    };
    connect(spin,&QSpinBox::valueChanged,spin,[this,key=field.key](int value){if(_model)_model->edit(key,value);});
    return {spin,refresh};
}
void NodeParameterForm::refreshText(const QString& key,QLineEdit* edit) {
    if(!_model)return;
    const auto* field=_model->field(key);if(!field || !field->read)return;
    const auto state=_model->editState(key);
    if(state.dirty){if(edit->text()!=state.input)edit->setText(state.input);edit->setModified(true);return;}
    edit->setText(std::get<QString>(field->read()));edit->setModified(false);
}
NodeParameterForm::ControlBinding NodeParameterForm::textControl(const Field& field) {
    auto* edit=new QLineEdit(this);
    connect(edit,&QLineEdit::textEdited,edit,[this,key=field.key](const QString& input){if(_model)_model->setDraftInput(key,input);});
    connect(edit,&QLineEdit::editingFinished,edit,[this,key=field.key,edit]{
        if(_model && edit->isModified() && _model->edit(key,edit->text()))edit->setModified(false);
    });
    return {edit,[this,key=field.key,edit]{refreshText(key,edit);}};
}
NodeParameterForm::ControlBinding NodeParameterForm::captionControl(const Field& field) {
    auto* label=new QLabel(this);label->setWordWrap(true);label->setTextFormat(Qt::PlainText);
    return {label,[this,key=field.key,label]{
        if(!_model)return;
        const auto* current=_model->field(key);if(!current || !current->read)return;
        const auto value=std::get<QString>(current->read());
        label->setText(current->translate_value?ui::text(value.toUtf8().constData()):value);
    }};
}
NodeParameterForm::ControlBinding NodeParameterForm::actionControl(const Field& field) {
    auto* button=ui::make<QPushButton>(field.label.toUtf8().constData(),this);
    connect(button,&QPushButton::clicked,button,[this,key=field.key]{if(_model)_model->edit(key,false);});
    return {button,{}};
}
NodeParameterForm::ControlBinding NodeParameterForm::createControl(const Field& field) {
    using E=NodeParameterModel::Editor;
    switch(field.editor){
    case E::Literal:return literalControl(field);
    case E::Choice:return choiceControl(field);
    case E::Boolean:return booleanControl(field);
    case E::Integer:return integerControl(field);
    case E::Text:return textControl(field);
    case E::Caption:return captionControl(field);
    default:return actionControl(field);
    }
}
void NodeParameterForm::configureControl(QWidget* control,const Field& field) {
    if(control->objectName().isEmpty())control->setObjectName(field.key);
    control->setMinimumWidth(0);
    auto policy=control->sizePolicy();policy.setHorizontalPolicy(_inline_form?QSizePolicy::Preferred:QSizePolicy::Ignored);control->setSizePolicy(policy);
}
void NodeParameterForm::configureRowGeometry(ui::FieldRow* row) {
    if(_inline_form)return;
    // Inspector width belongs to its splitter; stack the label and editor.
    auto* line=qobject_cast<QBoxLayout*>(row->layout()->itemAt(0)->layout());
    line->setDirection(QBoxLayout::TopToBottom);line->setAlignment(Qt::AlignTop);row->layout()->setAlignment(Qt::AlignTop);
    row->label()->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);row->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
}
void NodeParameterForm::bindRowCaption(ui::FieldRow* row,const Field& field) {
    if(field.caption_arguments.empty())return;
    auto caption=[row,field]{row->label()->setText(fieldCaption(field));};
    caption();_bindings.push_back(connect(&ui::Language::instance(),&ui::Language::changed,row,caption));
}
void NodeParameterForm::configureRow(ui::FieldRow* row,const Field& field) {
    configureRowGeometry(row);bindRowCaption(row,field);
    row->setObjectName(field.key+"_row");row->setHelp(field.help.isEmpty()?QString{}:ui::text(field.help.toUtf8().constData()));
}
QAction* NodeParameterForm::createPlacementAction(ui::CollapsibleSection* placement,const Field& field) {
    auto* show=new QAction(placement);show->setCheckable(true);show->setObjectName("showOnBlock_"+field.key);
    auto* check=new QCheckBox(placement);check->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
    check->setObjectName("fieldOnBlock_"+field.key);placement->contentLayout()->addWidget(check);
    auto caption=[show,check,field]{const auto text=fieldCaption(field);show->setText(text);check->setText(text);};
    caption();connect(&ui::Language::instance(),&ui::Language::changed,show,caption);
    connect(check,&QCheckBox::toggled,show,&QAction::setChecked);connect(show,&QAction::toggled,check,&QCheckBox::setChecked);
    return show;
}
QAction* NodeParameterForm::addPlacement(ui::CollapsibleSection* placement,ui::FieldRow* row,const Field& field) {
    if(!placement || !field.pinnable || field.primary)return nullptr;
    auto* show=createPlacementAction(placement,field);
    connect(show,&QAction::toggled,row,[this,key=field.key](bool value){if(_model && !_model->setOnBlock(key,value))emit _model->presentationChanged();});
    return show;
}
void NodeParameterForm::refreshPlacement(QAction* show,const QString& key,bool pinned) {
    if(!show)return;
    QSignalBlocker block(show);show->setChecked(pinned);
    if(auto* check=findChild<QCheckBox*>("fieldOnBlock_"+key)){QSignalBlocker checkBlock(check);check->setChecked(pinned);}
}
void NodeParameterForm::refreshVisibility(ui::FieldRow* row,const Field& field,bool pinned) {
    if(!_inline_form)return;
    const bool visible=pinned && !field.hide_inline;
    if(row->property("parameterVisibilityInitialized").toBool() && !row->isHidden()==visible)return;
    row->setProperty("parameterVisibilityInitialized",true);row->setVisible(visible);row->updateGeometry();
    if(layout())layout()->invalidate();updateGeometry();
}
void NodeParameterForm::refreshRow(ui::FieldRow* row,QWidget* control,QAction* show,const Field& field) {
    if(!_model)return;
    const auto* current=_model->field(field.key);control->setEnabled(current && (!current->available || current->available()));
    row->setError(fieldError(current,_model->editState(field.key)));
    const bool pinned=_model->onBlock(field.key);refreshPlacement(show,field.key,pinned);refreshVisibility(row,field,pinned);
}
void NodeParameterForm::bindControlRefresh(ui::FieldRow* row,const ControlBinding& control,const QString& key) {
    if(!control.refresh)return;
    _bindings.push_back(connect(_model,&NodeParameterModel::valuesChanged,row,control.refresh));
    _bindings.push_back(connect(_model,&NodeParameterModel::editStateChanged,row,[refresh=control.refresh,key](const QString& changed){if(changed==key)refresh();}));
    control.refresh();
}
void NodeParameterForm::bindRow(ui::FieldRow* row,ControlBinding control,QAction* show,const Field& field) {
    bindControlRefresh(row,control,field.key);
    auto state=[this,row,control=control.widget,show,field]{refreshRow(row,control,show,field);};
    _bindings.push_back(connect(_model,&NodeParameterModel::valuesChanged,row,state));
    _bindings.push_back(connect(_model,&NodeParameterModel::presentationChanged,row,state));
    _bindings.push_back(connect(_model,&NodeParameterModel::editStateChanged,row,[state,key=field.key](const QString& changed){if(changed==key)state();}));
    state();
}
QVBoxLayout* NodeParameterForm::createSection(QVBoxLayout* root,NodeParameterModel::Section sectionKind) {
    auto* section=new QWidget(this);section->setObjectName(QString("parameterSection%1").arg(int(sectionKind)));
    section->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);auto* content=new QVBoxLayout(section);
    content->setContentsMargins(0,0,0,0);content->setAlignment(Qt::AlignTop);
    auto* title=ui::make<QLabel>(NodeParameterModel::sectionTitle(sectionKind),section);title->setWordWrap(true);
    title->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);content->addWidget(title);root->addWidget(section);return content;
}
void NodeParameterForm::appendField(QVBoxLayout* root,QMap<int,QVBoxLayout*>& sections,ui::CollapsibleSection* placement,const Field& field) {
    _materialized_fields.insert(field.key);
    auto control=createControl(field);configureControl(control.widget,field);
    auto* row=new ui::FieldRow(field.label,control.widget,this);configureRow(row,field);
    auto* show=addPlacement(placement,row,field);
    if(_inline_form)root->addWidget(row);
    else {
        const int index=int(field.section);
        if(!sections.contains(index))sections[index]=createSection(root,field.section);
        sections[index]->addWidget(row);
    }
    bindRow(row,std::move(control),show,field);
}
void NodeParameterForm::appendComposite(QVBoxLayout* root) {
    if(!_inline_form && !_contracts_only && _composite_factory)if(auto* composite=_composite_factory(_model,this))root->addWidget(composite);
}
void NodeParameterForm::rebuild() {
    clearRows();if(!_model)return;
    auto* root=new QVBoxLayout(this);root->setContentsMargins(0,0,0,0);root->setSpacing(6);
    addBypass(root);
    auto* placement=createPlacement();
    appendComposite(root);
    appendFields(root,placement);
    if(placement)root->addWidget(placement);
    if(!_inline_form)root->addStretch(1);
    updateGeometry();
    emit layoutChanged();
}
void NodeParameterForm::appendFields(QVBoxLayout* root,ui::CollapsibleSection* placement) {
    QMap<int,QVBoxLayout*> sections;
    for(const auto& descriptor:_model->fields()) {
        // An edit can synchronously replace the model's field list.
        const auto field=descriptor;
        QElapsedTimer timer;if(qEnvironmentVariableIsSet("CORE_APP_TRACE_SAVES"))timer.start();
        if(showsField(field))appendField(root,sections,placement,field);
        if(timer.isValid() && timer.nsecsElapsed()>5000000)
            qInfo().noquote()<<"BARELIEF_INSPECTOR_FIELD"<<field.key<<"ms="<<timer.nsecsElapsed()/1e6;
    }
}
