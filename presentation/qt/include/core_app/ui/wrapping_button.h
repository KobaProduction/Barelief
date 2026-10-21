#pragma once
#include <QPushButton>
namespace ui {
// Native text action with an intrinsic word-sized minimum and wrapping height.
class WrappingButton final : public QPushButton {
public:
    explicit WrappingButton(const QString& text=QString{},QWidget* parent=nullptr);
    bool hasHeightForWidth() const override { return true; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    int heightForWidth(int width) const override;
protected:
    void paintEvent(QPaintEvent* event) override;
private:
    QSize buttonSize(const QSize& contents) const;
};
}
