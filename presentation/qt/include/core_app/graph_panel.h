#pragma once
#include <QWidget>
#include <QJsonObject>
#include <functional>
#include "visualization/abstract/abs.h"

class QComboBox;
class QStackedWidget;
class QScrollArea;
class QFormLayout;
class NumericImageView;
class NumericPieView;
class NumericMetricsView;
class SpectrogramGraph;

// View preferences belong to the GUI, separate from processing graph parameters.
class GraphPanel final : public QWidget {
    Q_OBJECT
public:
    GraphPanel(const QString &id, AbstractChartView *chart, QJsonObject preferences, QWidget *parent,SpectrogramGraph* supplied_surface=nullptr);
    // Selects an explicit presentation plane; never edits processing parameters.
    void set_numeric_plane_count(size_t count,size_t selected);
    void set_render_visible(bool visible);
    void restore_presentation(const QJsonObject &preferences);
    AbstractChartView* active_chart() const;
    SpectrogramGraph* active_spectrogram() const;
    NumericImageView* image_view() const;
    NumericPieView* pie_view() const;
    NumericMetricsView* metrics_view() const {return _metrics;}
    QString display_mode() const {return _display_mode;}
    void set_display_mode(const QString& mode);
    void set_render_fps(int fps);
    void set_axis_precision(int precision);
signals:
    void presentation_changed(const QString &id, const QJsonObject &preferences);
    void channel_requested(int channel);
    void renderer_changed();
private:
    void add_axis_display_controls(QFormLayout* form,QWidget* parent);
    void apply_axis_display_modes(const QJsonObject& preferences);
    void refresh_axis_display_controls();
    QString _frequency_axis_mode="coordinates",_time_axis_mode="coordinates";
    QComboBox *_channel=nullptr;
    QComboBox *_surface_channel=nullptr;
    size_t _plane_count=0,_selected_plane=0;
    QTimer *_information_timer=nullptr;
    std::function<void(const QJsonObject&)> _restore_presentation;
    AbstractChartView* _chart=nullptr;
    SpectrogramGraph* _surface=nullptr;
    NumericImageView* _image=nullptr;
    NumericPieView* _pie=nullptr;
    NumericMetricsView* _metrics=nullptr;
    QStackedWidget* _renderers=nullptr;
    QScrollArea* _settings=nullptr;
    QComboBox* _mode=nullptr;
    QString _display_mode="line",_default_display_mode="line",_id;
    QStringList _allowed_modes;
    QJsonObject _preferences;
    bool _render_visible=true;
    int _render_fps=60;
    int _axis_precision=0;
    bool _surface_integrated=false;
};
