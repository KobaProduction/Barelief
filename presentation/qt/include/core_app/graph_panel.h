#pragma once
#include <QWidget>
#include <QJsonObject>
#include <visualization/quick/numeric_render_ports.h>
class AbstractChartView;
class NumericImageView;
class NumericPieView;
class NumericMetricsView;
class SpectrogramGraph;
class QStackedWidget;
class QQuickWidget;
class QSplitter;
class QTimer;
class GraphPanel final : public QWidget {
    Q_OBJECT
    Q_PROPERTY(QVariantMap preferences READ preferences NOTIFY state_changed)
    Q_PROPERTY(QStringList modes READ modes CONSTANT)
    Q_PROPERTY(QString displayMode READ display_mode NOTIFY state_changed)
    Q_PROPERTY(int planeCount READ plane_count NOTIFY state_changed)
    Q_PROPERTY(int selectedPlane READ selected_plane NOTIFY state_changed)
    Q_PROPERTY(bool physicalFrequency READ physical_frequency NOTIFY state_changed)
    Q_PROPERTY(bool physicalTime READ physical_time NOTIFY state_changed)
    Q_PROPERTY(QString diagnostic READ diagnostic NOTIFY state_changed)
    Q_PROPERTY(QString information READ information NOTIFY state_changed)
    Q_PROPERTY(int renderFps READ render_fps NOTIFY state_changed)
    Q_PROPERTY(int axisPrecision READ axis_precision NOTIFY state_changed)
    Q_PROPERTY(QVariantList scalarInputs READ scalar_inputs NOTIFY state_changed)
    Q_PROPERTY(bool canAddScalarInput READ can_add_scalar_input NOTIFY state_changed)
public:
    GraphPanel(const QString&,AbstractChartView*,QJsonObject,QWidget*,SpectrogramGraph* supplied_surface=nullptr);
    ~GraphPanel() override;
    void set_numeric_plane_count(size_t count,size_t selected);
    void set_render_visible(bool visible);
    void restore_presentation(const QJsonObject&);
    AbstractChartView* active_chart() const;
    SpectrogramGraph* active_spectrogram() const;
    SpectrogramGraph* owned_surface() const {return _surface;}
    NumericImageView* image_view() const;
    NumericPieView* pie_view() const;
    NumericMetricsView* metrics_view() const {return _metrics;}
    presentation::NumericRenderTarget render_target(bool visible);
    presentation::NumericRenderTarget all_render_targets();
    QString display_mode() const {return _display_mode;}
    void set_display_mode(const QString&);
    void set_render_fps(int);
    void set_axis_precision(int);
    QVariantMap preferences() const {return _preferences.toVariantMap();}
    QStringList modes() const {return _allowed_modes;}
    int plane_count() const {return int(_plane_count);}
    int selected_plane() const {return int(_selected_plane);}
    bool physical_frequency() const;
    bool physical_time() const;
    QString diagnostic() const {return _diagnostic;}
    QString information() const {return _information;}
    int render_fps() const {return _render_fps;}
    int axis_precision() const {return _axis_precision;}
    QVariantList scalar_inputs() const {return _scalar_inputs;}
    bool can_add_scalar_input() const {return _can_add_scalar_input;}
    void set_scalar_inputs(QVariantList inputs,bool canAdd){if(_scalar_inputs==inputs && _can_add_scalar_input==canAdd)return;_scalar_inputs=std::move(inputs);_can_add_scalar_input=canAdd;emit state_changed();}
    Q_INVOKABLE void changePreference(const QString& key,const QVariant& value);
    Q_INVOKABLE void selectMode(const QString& mode){set_display_mode(mode);}
    Q_INVOKABLE void selectPlane(int plane);
    Q_INVOKABLE void changeFps(int fps){set_render_fps(fps);}
    Q_INVOKABLE void changePrecision(int precision){set_axis_precision(precision);}
    Q_INVOKABLE void rename(){emit rename_requested();}
    Q_INVOKABLE void addScalarInput(){if(_can_add_scalar_input)emit add_scalar_input_requested();}
    Q_INVOKABLE void removeScalarInput(int port){emit remove_scalar_input_requested(port);}
    Q_INVOKABLE void nameScalarInput(int port,const QString& name){emit scalar_input_name_requested(port,name);}
    Q_INVOKABLE void enableScalarInput(int port,bool enabled){emit scalar_input_enabled_requested(port,enabled);}
    Q_INVOKABLE QString modeLabel(const QString&) const;
signals:
    void presentation_changed(const QString&,const QJsonObject&);
    void channel_requested(int);
    void renderer_changed();
    void rename_requested();
    void add_scalar_input_requested();
    void remove_scalar_input_requested(int port);
    void scalar_input_name_requested(int port,const QString& name);
    void scalar_input_enabled_requested(int port,bool enabled);
    void state_changed();
private:
    void apply_preferences();
    void initialize_surface();
    void restore_surface_preferences();
    void initialize_ports();
    void refresh_captions();
    void refresh_axis_display_controls(){emit state_changed();}
    QString caption(QObject*,const char*,QStringList,bool,bool);
    AbstractChartView* _chart=nullptr;
    SpectrogramGraph* _surface=nullptr;
    NumericImageView* _image=nullptr;
    NumericPieView* _pie=nullptr;
    NumericMetricsView* _metrics=nullptr;
    QStackedWidget* _renderers=nullptr;
    QQuickWidget* _settings=nullptr;
    QSplitter* _splitter=nullptr;
    QTimer* _information_timer=nullptr;
    QString _display_mode="line",_default_display_mode="line",_id;
    QStringList _allowed_modes;
    QJsonObject _preferences;
    bool _render_visible=true,_restoring=false,_surface_integrated=false;
    size_t _plane_count=0,_selected_plane=0;
    int _render_fps=60,_axis_precision=0;
    QString _diagnostic,_information,_x_title_key="Value index (X)",_y_title_key="Value (Y)";
    QVariantList _scalar_inputs;
    bool _can_add_scalar_input=false;
    presentation::NumericChartPort _chart_port;
    presentation::NumericSurfacePort _surface_port;
    presentation::NumericImagePort _image_port;
    presentation::NumericPiePort _pie_port;
    presentation::NumericMetricsPort _metrics_port;
    presentation::NumericCaptionPort _caption_port;
};
