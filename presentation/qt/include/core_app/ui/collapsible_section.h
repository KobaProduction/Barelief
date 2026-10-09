#pragma once
#include <QWidget>
#include <QString>
class QToolButton;
class QVBoxLayout;
namespace ui {
class CollapsibleSection final : public QWidget {
    Q_OBJECT
  public:
    explicit CollapsibleSection(const QString &title_key, QWidget *parent = nullptr);
    explicit CollapsibleSection(const char *title_key, QWidget *parent = nullptr);
    [[nodiscard]] QVBoxLayout *contentLayout() const { return _layout; }
    void setExpanded(bool expanded);
    [[nodiscard]] bool isExpanded() const;
  signals:
    void expandedChanged(bool expanded);
  private:
    QToolButton *_toggle;
    QWidget *_content;
    QVBoxLayout *_layout;
};
}
