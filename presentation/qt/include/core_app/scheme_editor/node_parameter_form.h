#pragma once
#include <QWidget>
#include <QPointer>
#include <QMap>
#include <vector>
#include <core_app/scheme_editor/node_parameter_model.h>
class QVBoxLayout;
class QComboBox;
class QCheckBox;
class QAction;
class QLineEdit;
namespace ui {class FieldRow;class CollapsibleSection;}
class NodeParameterForm final : public QWidget {
  public:
    using CompositeFactory=std::function<QWidget*(NodeParameterModel*,QWidget*)>;
    NodeParameterForm(NodeParameterModel *, bool inline_form, QWidget *parent = nullptr, bool contracts_only = false,CompositeFactory composite_factory={});

  private:
    QPointer<NodeParameterModel> _model;
    bool _inline_form;
    bool _contracts_only;
    bool _rebuild_pending = false;
    bool _placement_expanded = false;
    CompositeFactory _composite_factory;
    std::vector<QMetaObject::Connection> _bindings;
    using Field=NodeParameterModel::Field;
    struct ControlBinding {QWidget* widget=nullptr;std::function<void()> refresh;};
    void scheduleRebuild();
    void clearRows();
    void addBypass(QVBoxLayout*);
    void bindBypass(QCheckBox*);
    ui::CollapsibleSection* createPlacement();
    bool showsField(const Field&) const;
    ControlBinding createControl(const Field&);
    ControlBinding literalControl(const Field&);
    ControlBinding choiceControl(const Field&);
    ControlBinding booleanControl(const Field&);
    ControlBinding integerControl(const Field&);
    ControlBinding textControl(const Field&);
    ControlBinding captionControl(const Field&);
    ControlBinding actionControl(const Field&);
    void refreshChoices(const QString&,QComboBox*);
    void editChoice(const QString&,int);
    void refreshText(const QString&,QLineEdit*);
    void configureControl(QWidget*,const Field&);
    void configureRowGeometry(ui::FieldRow*);
    void bindRowCaption(ui::FieldRow*,const Field&);
    void appendField(QVBoxLayout*,QMap<int,QVBoxLayout*>&,ui::CollapsibleSection*,const Field&);
    QAction* createPlacementAction(ui::CollapsibleSection*,const Field&);
    void bindControlRefresh(ui::FieldRow*,const ControlBinding&,const QString&);
    void configureRow(ui::FieldRow*,const Field&);
    QAction* addPlacement(ui::CollapsibleSection*,ui::FieldRow*,const Field&);
    void refreshRow(ui::FieldRow*,QWidget*,QAction*,const Field&);
    void refreshPlacement(QAction*,const QString&,bool);
    void refreshVisibility(ui::FieldRow*,const Field&,bool);
    void bindRow(ui::FieldRow*,ControlBinding,QAction*,const Field&);
    QVBoxLayout* createSection(QVBoxLayout*,NodeParameterModel::Section);
    void rebuild();
    void appendComposite(QVBoxLayout*);
    void appendFields(QVBoxLayout*,ui::CollapsibleSection*);
};
