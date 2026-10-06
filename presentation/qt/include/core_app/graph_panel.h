#pragma once
#include <QWidget>
#include <QJsonObject>
#include "visualization/abstract/abs.h"

class QComboBox;

// View preferences belong to the GUI, separate from processing graph parameters.
class GraphPanel final : public QWidget {
    Q_OBJECT
public:
    GraphPanel(const QString &id, AbstractChartView *chart, QJsonObject preferences, QWidget *parent);
    void set_processing_choices(int channel, int merge);
    void set_render_visible(bool visible);
signals:
    void presentation_changed(const QString &id, const QJsonObject &preferences);
    void channel_requested(int channel);
    void merge_requested(int merge);
private:
    QComboBox *_channel=nullptr, *_merge=nullptr;
    QTimer *_information_timer=nullptr;
};
