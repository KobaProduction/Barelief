#pragma once
#include <QLabel>

namespace ui {
class ValidationMessage final : public QLabel {
  public:
    explicit ValidationMessage(QWidget *parent = nullptr);
    void setMessage(const QString &message);
};
}
