#ifndef SPECTROGRAM_GRAPH_H
#define SPECTROGRAM_GRAPH_H

#include <QWidget>
#include <QQuaternion>
#include <QVector3D>
#include <QTimer>
#include <QJsonObject>
#include <QSet>
#include <QElapsedTimer>
#include <QPair>
#include <QVariantMap>
#include <vector>
#include <cmath>
#include <limits>
#include <cstdint>
#include <algorithm>
#include "visualization/camera_controls.h"
#include "visualization/numeric_matrix_snapshot.h"
#include "visualization/amplitude_range.h"
#include "visualization/spectrograms/spectrogram_scene_geometry.h"

#include "visualization/abstract/mixins.h"

class SpectrogramRenderer;
#include "visualization/spectrogram_axis_metadata.h"
class QQuickWidget;
class SurfaceQuickItem;
class SpectrogramGraph : public QWidget {
    Q_OBJECT
    Q_PROPERTY(QVariantMap quickState READ quick_state NOTIFY quick_state_changed)
    Q_PROPERTY(QVariantMap overlay READ overlay NOTIFY overlay_changed)

  public:
    explicit SpectrogramGraph(QWidget *parent = nullptr);

    ~SpectrogramGraph() override;

    QVariantMap quick_state() const;
    QVariantMap overlay() const {return _overlay;}
    Q_INVOKABLE void quick_action(const QString &action, const QVariant &value = {});
    Q_INVOKABLE void quick_range(int axis,double low,double high,bool automatic);
    Q_INVOKABLE void quick_pointer(int type,double x,double y,int button,int buttons,int modifiers);
    Q_INVOKABLE void quick_wheel(double horizontal,double vertical,bool pixels);
    Q_INVOKABLE void quick_key(bool pressed,int key,int modifiers,bool repeat);
    Q_INVOKABLE void quick_viewport(double width,double height);
    Q_INVOKABLE void quick_cancel_input();
    void accept_render_feedback(const QVariantMap &feedback);
    bool initialize(const QSize &minimumGraphSize, const QSize &maximumGraphSize);

    void update(const std::vector<std::vector<float>> &spectrogram);
    // Typed pipeline history is [frame][band]; legacy update accepts [band][frame].
    void update_frame_history(const std::vector<std::vector<float>> &history);
    // Immutable band/frame view of GUI-owned storage; no transpose or copy.
    void update_snapshot(NumericMatrixSnapshot snapshot);
    const void* projection_storage_identity() const noexcept {return _spectrogram.storage_identity();}
    // App-owned projection telemetry before OpenGL upload; no screen capture.
    size_t projected_history_frames() const noexcept {return _spectrogram.empty()?0:_spectrogram.front().size();}
    size_t projected_history_bands() const noexcept {return _spectrogram.size();}
    std::uint64_t history_projection_revision() const noexcept {return _history_projection_revision;}
    bool projected_history_sample(size_t frame,size_t band,float& value) const noexcept {
        if(band>=_spectrogram.size() || frame>=_spectrogram[band].size())return false;
        value=_spectrogram[band][frame];return true;
    }
    void set_history_dimensions(size_t capacity, size_t bands);
    void set_paused(bool paused);
    bool paused() const noexcept {return _pause;}
    const SpectrogramAxisMetadata& projected_axis_metadata() const noexcept {return _axis_metadata;}
    const SpectrogramAxisMetadata& source_axis_metadata() const noexcept {return _source_axis_metadata;}
    QString projected_status() const {return _projection_status;}
    size_t projected_history_capacity() const noexcept {return _history_capacity;}
    SpectrogramSceneGeometry projected_scene_geometry() const;
    bool set_axis_metadata(const SpectrogramAxisMetadata&);
    void set_axis_display_modes(const QString& frequencyMode,const QString& timeMode);
    void set_projection_status(const QString&);
    void set_render_fps(int fps);
    void set_render_visible(bool visible);
    void set_y_range(float minimum,float maximum);
    QPair<float,float> y_range() const;
    QJsonObject surface_settings() const;
    void set_surface_settings(const QJsonObject &options);
    qreal display_fps() const;
    QVariantMap axis_layout_telemetry() const;

    static constexpr float maximum_camera_distance = 30.0f / 3.0f;
    static constexpr float maximum_camera_zoom = 1666.6667f;
    void set_side_panel_ratio(float ratio);
    void set_camera_controls(const QJsonObject &controls);
    void reset_camera();
    static QVector3D camera_translation(float yaw, float side, float up, float horizontal_ratio, float aspect);
    QJsonObject presentation_state() const;
    void set_presentation_state(const QJsonObject &state);
    QJsonObject camera_state() const;
    void set_camera_state(const QJsonObject &state);

  signals:
    void quick_state_changed();
    void overlay_changed();
    void axis_metadata_changed();
    void presentation_changed(const QJsonObject &state);
    void surface_settings_error(const QString &message);
    void camera_changed(const QJsonObject &state);
    void side_panel_resized(float ratio);
    void y_range_changed(float minimum,float maximum);
    void surface_settings_changed(QJsonObject options);
    void surface_settings_updated(QJsonObject options);
    void edit_color_scale_requested();

  protected:
    virtual std::int64_t amplitude_clock() const {return AmplitudeRange::now();}
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

  private:
    void _move_camera();
    void _camera_changed();
    void _stop_camera();
    void _refresh_camera_keys(Qt::KeyboardModifiers modifiers);
    QQuickWidget *_quick_view = nullptr;
    SurfaceQuickItem *_quick_item = nullptr;
    QVariantMap _overlay;
    float _side_panel_ratio = 0.25f;
    QJsonObject _camera_state, _presentation_state;
    QJsonObject _surface_settings;
    size_t _history_capacity=0, _history_bands=0;
    SpectrogramAxisMetadata _axis_metadata;
    SpectrogramAxisMetadata _source_axis_metadata;
    QString _frequency_axis_mode="coordinates",_time_axis_mode="coordinates";
    QString _projection_status;
    int _render_fps=60;
    bool _render_visible=true;
    QJsonObject _camera_controls = CameraControls::defaults();
    QJsonObject _default_camera_state;
    QSet<QString> _camera_keys;
    QHash<int, QPair<int,int>> _held_camera_keys;
    QTimer _camera_timer;
    QElapsedTimer _camera_clock;

    bool _initialization_attempted = false;
    SpectrogramRenderer *m_spectrogram_graph = nullptr;
    QVector3D _position{0, 1.6f, 2.8f};
    QQuaternion _orientation = QQuaternion::fromEulerAngles(-30, 0, 0);
    float _movement_speed = 3.0f;
    QPoint _mouse_position;
    QWidget *m_container = nullptr;

    struct PendingProjection {
        size_t capacity=0,bands=0;
        SpectrogramAxisMetadata axes;
        QString status;
        NumericMatrixSnapshot data;
    };
    PendingProjection _pending_projection;
    bool _pause = false;
    NumericMatrixSnapshot _spectrogram;
    std::uint64_t _history_projection_revision=0;
    AmplitudeRange _amplitude_range;
    bool _observe_amplitude_range();
};

#endif
