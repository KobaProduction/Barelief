#include "literal_edit_binding.h"
#include "core_app/scheme_editor/node_parameter_model.h"
#include "core_app/scheme_editor/typed_parameter_helper.h"
#include <QComboBox>
#include <QLineEdit>
#include <QPointer>
#include <QSignalBlocker>
namespace numeric_editor {
namespace {
int current_literal_type(QComboBox* selector,int fixed_type) {return selector?selector->currentIndex():fixed_type;}
void bind_literal_signals(const QPointer<NodeParameterModel>& guarded,const QString& key,QComboBox* type,QLineEdit* edit,int fixed_type=0) {
    QObject::connect(edit,&QLineEdit::textEdited,edit,[guarded,key,type,fixed_type](const QString& input){
        if(guarded)guarded->setDraftInput(key,input,current_literal_type(type,fixed_type));
    });
    QObject::connect(edit,&QLineEdit::editingFinished,edit,[guarded,key,type,edit,fixed_type]{
        // QLineEdit's modified flag also covers explicit editor integrations
        // that set text without emitting textEdited. editLiteral owns draft
        // registration and exact no-op detection; untouched editors stay idle.
        if(!guarded || !edit->isModified())return;
        if(guarded->editLiteral(key,edit->text(),current_literal_type(type,fixed_type)))edit->setModified(false);
    });
    if(type)QObject::connect(type,&QComboBox::currentIndexChanged,edit,[guarded,key,edit](int index){
        if(guarded && guarded->editLiteral(key,edit->text(),index))edit->setModified(false);
    });
}

void refresh_clean_literal(const NodeParameterModel::Field& field,QComboBox* type,QLineEdit* edit) {
    const auto value=std::get<signal_contracts::NumericLiteral>(field.read());
    // A schema integer editor can store an unsigned literal internally.
    // Keep its declared editor type rather than silently selecting the
    // hidden unsigned combo item and rejecting the next user edit.
    if(type)type->setCurrentIndex(field.literal_type.value_or(int(value.index())));
    edit->setText(literal_text(value));
    edit->setModified(false);
}

void refresh_literal_input(const QPointer<NodeParameterModel>& guarded,const QString& key,QComboBox* type,QLineEdit* edit) {
    if(!guarded)return;
    const auto* field=guarded->field(key);if(!field || !field->read)return;
    const auto state=guarded->editState(key);
    const QSignalBlocker block(type);
    if(state.dirty){
        if(type)type->setCurrentIndex(field->literal_type.value_or(state.literal_type));
        if(edit->text()!=state.input)edit->setText(state.input);
        edit->setModified(true);
        return;
    }
    refresh_clean_literal(*field,type,edit);
}
}

std::function<void()> bind_literal_input(NodeParameterModel* model,const QString& key,QComboBox* type,QLineEdit* edit) {
    const QPointer<NodeParameterModel> guarded(model);
    bind_literal_signals(guarded,key,type,edit);
    return [guarded,key,type,edit]{refresh_literal_input(guarded,key,type,edit);};
}
std::function<void()> bind_literal_input(NodeParameterModel* model,const QString& key,int type,QLineEdit* edit) {
    const QPointer<NodeParameterModel> guarded(model);
    bind_literal_signals(guarded,key,nullptr,edit,type);
    return [guarded,key,edit]{refresh_literal_input(guarded,key,nullptr,edit);};
}
}
