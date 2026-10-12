#include "literal_edit_binding.h"
#include "core_app/scheme_editor/node_parameter_model.h"
#include "core_app/scheme_editor/typed_parameter_helper.h"
#include <QComboBox>
#include <QLineEdit>
#include <QPointer>
#include <QSignalBlocker>
namespace numeric_editor {
std::function<void()> bind_literal_input(NodeParameterModel* model,const QString& key,QComboBox* type,QLineEdit* edit) {
    const QPointer<NodeParameterModel> guarded(model);
    QObject::connect(edit,&QLineEdit::textEdited,edit,[guarded,key,type](const QString& input){
        if(guarded)guarded->setDraftInput(key,input,type->currentIndex());
    });
    QObject::connect(edit,&QLineEdit::editingFinished,edit,[guarded,key,type,edit]{
        // QLineEdit's modified flag also covers explicit editor integrations
        // that set text without emitting textEdited. editLiteral owns draft
        // registration and exact no-op detection; untouched editors stay idle.
        if(guarded && edit->isModified())
            if(guarded->editLiteral(key,edit->text(),type->currentIndex()))edit->setModified(false);
    });
    QObject::connect(type,&QComboBox::currentIndexChanged,edit,[guarded,key,edit](int index){
        if(guarded && guarded->editLiteral(key,edit->text(),index))edit->setModified(false);
    });
    return [guarded,key,type,edit]{
        if(!guarded)return;
        const auto* field=guarded->field(key);if(!field || !field->read)return;
        const auto state=guarded->editState(key);
        const QSignalBlocker block(type);
        if(state.dirty){
            type->setCurrentIndex(state.literal_type);
            if(edit->text()!=state.input)edit->setText(state.input);
            edit->setModified(true);
        }else{
            const auto value=std::get<signal_contracts::NumericLiteral>(field->read());
            type->setCurrentIndex(int(value.index()));
            edit->setText(literal_text(value));edit->setModified(false);
        }
    };
}
}
