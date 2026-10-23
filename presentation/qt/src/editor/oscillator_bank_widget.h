#pragma once
#include <QTableWidget>
#include <QPointer>
class NodeParameterModel;
// A composite parameter editor. All edits go through NodeParameterModel;
// the widget does not own oscillator parameters, validation or undo state.
class OscillatorBankWidget final : public QTableWidget {
public:
    explicit OscillatorBankWidget(NodeParameterModel*,QWidget* parent=nullptr);
private:
    void configureTable();
    void setupRow(int row,QPointer<NodeParameterModel> model,const QStringList& suffixes);
    void setupRowControls(int row,QPointer<NodeParameterModel> model,const QString& prefix);
    void setupValueCell(int row,int column,QPointer<NodeParameterModel> model,const QString& key);
};
