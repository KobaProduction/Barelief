#ifndef SURFACEGRAPH_H
#define SURFACEGRAPH_H

#include <QtDataVisualization/q3dsurface.h>
#include <QJsonObject>
#include <QSet>
#include <QElapsedTimer>
#include <QSplitter>
#include "visualization/camera_controls.h"

#include "visualization/surfaces/surfacegraphmodifier.h"
#include "visualization/abstract/mixins.h"

class SurfaceGraph final : public QWidget {
    Q_OBJECT

public:
    explicit SurfaceGraph(QWidget *parent = nullptr);

    ~SurfaceGraph() override;

    bool initialize(const QSize &minimumGraphSize, const QSize &maximumGraphSize);

    void update(const std::vector<std::vector<float> > &spectrogram);

    static constexpr float maximum_camera_zoom = 5000.0f;
    void set_side_panel_ratio(float ratio);
    void set_camera_controls(const QJsonObject &controls);
    void reset_camera();
    static QVector3D camera_translation(float yaw, float side, float up, float horizontal_ratio, float aspect);
    QJsonObject camera_state() const;
    void set_camera_state(const QJsonObject &state);

signals:
    void camera_changed(const QJsonObject &state);
    void side_panel_resized(float ratio);

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
    QJsonObject _camera_state;
    QJsonObject _camera_controls = CameraControls::defaults();
    QJsonObject _default_camera_state;
    QSet<QString> _camera_keys;
    QHash<int,QString> _held_camera_keys;
    QTimer _camera_timer;
    QElapsedTimer _camera_clock;

    bool _initialization_attempted = false;
    Q3DSurface *m_surfaceGraph = nullptr;
    QWidget *m_container = nullptr;
    SurfaceGraphModifier *_modifier = nullptr;

    bool _need_update = false;
    bool _pause = false;
    std::vector<std::vector<float>> _spectrogram;
};

#endif
