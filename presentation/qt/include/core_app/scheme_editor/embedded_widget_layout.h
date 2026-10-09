#pragma once
#include <QWidget>
#include <QLayout>
#include <algorithm>

inline int embeddedContentWidth(QWidget *body,int preferred) {
    auto* layout=body->layout();
    return std::max(preferred,layout?layout->totalMinimumSize().width():0);
}

inline int embeddedContentHeight(QWidget *body) {
    auto* layout=body->layout();if(!layout)return body->sizeHint().height();
    layout->activate();
    const int wrapped=layout->hasHeightForWidth()?layout->totalHeightForWidth(body->width()):0;
    return std::max({0,wrapped,layout->totalMinimumSize().height(),layout->totalSizeHint().height()});
}
