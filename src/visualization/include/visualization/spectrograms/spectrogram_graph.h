#ifndef SPECTROGRAM_GRAPH_H
#define SPECTROGRAM_GRAPH_H

#include <QWidget>
#include <QQuaternion>
#include <QVector3D>
#include <QTimer>
#include <QJsonObject>
#include <QSet>
#include <QElapsedTimer>
#include <QSplitter>
#include <QPair>
#include "visualization/camera_controls.h"

#include "visualization/abstract/mixins.h"

class SpectrogramRenderer;
class SpectrogramGraph final : public QWidget {
    Q_OBJECT

  public:
    explicit SpectrogramGraph(QWidget *parent = nullptr);

    ~SpectrogramGraph() override;

    bool initialize(const QSize &minimumGraphSize, const QSize &maximumGraphSize);

    void update(const std::vector<std::vector<float>> &spectrogram);
    // Typed pipeline history is [frame][band]; legacy update accepts [band][frame].
    void update_frame_history(const std::vector<std::vector<float>> &history);
    void set_history_dimensions(size_t capacity, size_t bands);
    void set_render_fps(int fps);
    void set_render_visible(bool visible);
    void set_y_range(float minimum,float maximum);
    QPair<float,float> y_range() const;
    QJsonObject surface_settings() const;
    void set_surface_settings(const QJsonObject &options);
    qreal display_fps() const;

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
    void presentation_changed(const QJsonObject &state);
    void surface_settings_error(const QString &message);
    void camera_changed(const QJsonObject &state);
    void side_panel_resized(float ratio);
    void y_range_changed(float minimum,float maximum);
    void surface_settings_changed(QJsonObject options);
    void surface_settings_updated(QJsonObject options);
    void edit_color_scale_requested();

  protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;

  private:
    void _move_camera();
    void _camera_changed();
    void _stop_camera();
    QSplitter *_splitter = nullptr;
    float _side_panel_ratio = 0.25f;
    QJsonObject _camera_state, _presentation_state;
    QJsonObject _surface_settings;
    size_t _history_capacity=0, _history_bands=0;
    int _render_fps=60;
    bool _render_visible=true;
    QJsonObject _camera_controls = CameraControls::defaults();
    QJsonObject _default_camera_state;
    QSet<QString> _camera_keys;
    QHash<int, QString> _held_camera_keys;
    QTimer _camera_timer;
    QElapsedTimer _camera_clock;

    bool _initialization_attempted = false;
    SpectrogramRenderer *m_spectrogram_graph = nullptr;
    QVector3D _position{0, 1.6f, 2.8f};
    QQuaternion _orientation = QQuaternion::fromEulerAngles(-30, 0, 0);
    float _movement_speed = 3.0f;
    QPoint _mouse_position;
    QWidget *m_container = nullptr;

    bool _pause = false;
    std::vector<std::vector<float>> _spectrogram;
};

#endif
