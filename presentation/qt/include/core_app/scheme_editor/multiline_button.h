#pragma once
#include <QPushButton>
#include <QStylePainter>
#include <QStyleOptionButton>
#include <algorithm>

// Native button frame/palette, with content-driven multiline geometry.
class MultilineButton final : public QPushButton {
public:
    explicit MultilineButton(QWidget *parent):QPushButton(parent) {
        QSizePolicy policy(QSizePolicy::Ignored,QSizePolicy::Preferred);
        policy.setHeightForWidth(true);setSizePolicy(policy);
    }
    int heightForWidth(int width) const override {
        const int margin=style()->pixelMetric(QStyle::PM_ButtonMargin,nullptr,this)+4;
        const auto bounds=fontMetrics().boundingRect(QRect(0,0,std::max(1,width-2*margin),100000),
            Qt::TextWordWrap | Qt::AlignCenter,text());
        return bounds.height()+2*margin;
    }
    QSize sizeHint() const override {
        const int margin=style()->pixelMetric(QStyle::PM_ButtonMargin,nullptr,this)+4;
        const int naturalWidth=fontMetrics().size(Qt::TextShowMnemonic,text()).width()+2*margin;
        return {naturalWidth,heightForWidth(naturalWidth)};
    }
    QSize minimumSizeHint() const override {return {0,sizeHint().height()};}
protected:
    void paintEvent(QPaintEvent*) override {
        QStylePainter painter(this);QStyleOptionButton option;initStyleOption(&option);
        option.text.clear();painter.drawControl(QStyle::CE_PushButton,option);
        const int margin=style()->pixelMetric(QStyle::PM_ButtonMargin,nullptr,this)+4;
        style()->drawItemText(&painter,rect().adjusted(margin,margin,-margin,-margin),
            Qt::TextWordWrap | Qt::AlignCenter,palette(),isEnabled(),text(),QPalette::ButtonText);
    }
};
