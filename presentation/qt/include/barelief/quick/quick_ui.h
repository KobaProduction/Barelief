#pragma once
#include <QObject>
#include <QVariantMap>
#include <QQuickWidget>
namespace ui::quick {
class UiBridge final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap palette READ palette NOTIFY changed)
    Q_PROPERTY(int revision READ revision NOTIFY changed)
public:
    explicit UiBridge(QObject* parent=nullptr);
    QVariantMap palette() const {return palette_;}
    int revision() const {return revision_;}
    Q_INVOKABLE QString text(const QString& source) const;
signals:
    void changed();
protected:
    bool eventFilter(QObject* watched,QEvent* event) override;
private:
    void schedule_refresh(bool languageChanged=false);
    QVariantMap palette_;
    int revision_=0;
    bool refresh_queued_=false,language_changed_=false;
};
// Call before constructing the first Quick widget (desktop composition root).
void configure();
UiBridge* install(QQuickWidget* widget);
void initialize(QQuickWidget* widget,QObject* model,const QUrl& source);
}
