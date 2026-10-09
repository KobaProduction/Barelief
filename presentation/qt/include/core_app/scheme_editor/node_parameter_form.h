#pragma once
#include <QWidget>
#include <QPointer>
#include <vector>
class NodeParameterModel;
class NodeParameterForm final : public QWidget {
  public:
    NodeParameterForm(NodeParameterModel *, bool inline_form, QWidget *parent = nullptr);

  private:
    QPointer<NodeParameterModel> _model;
    bool _inline_form;
    bool _rebuild_pending = false;
    std::vector<QMetaObject::Connection> _bindings;
    void rebuild();
};
