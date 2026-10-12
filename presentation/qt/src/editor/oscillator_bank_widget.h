#pragma once
#include <QTableWidget>
class NodeParameterModel;
// A composite parameter editor. All edits go through NodeParameterModel;
// the widget does not own oscillator parameters, validation or undo state.
class OscillatorBankWidget final : public QTableWidget {
public:
    explicit OscillatorBankWidget(NodeParameterModel*,QWidget* parent=nullptr);
};
