#pragma once
#include <QWidget>
#include <QString>
class QLabel;
class QHBoxLayout;
namespace ui {
class ValidationMessage;
class FieldRow final : public QWidget {
  public:
    FieldRow(const QString &label_key, QWidget *editor, QWidget *parent = nullptr);
    FieldRow(const char *label_key, QWidget *editor, QWidget *parent = nullptr);
    void setHelp(const QString &help);
    void setError(const QString &error);
    void setResetControl(QWidget *control);
    [[nodiscard]] QLabel *label() const { return _label; }
    [[nodiscard]] QWidget *editor() const { return _editor; }
  private:
    QWidget *setupLabel(const QString &key, QWidget *editor);
    void setupAccessibility(QWidget *focus_target);
    QLabel *_label;
    QWidget *_editor;
    QLabel *_help;
    ValidationMessage *_error;
    QHBoxLayout *_line;
};
}
