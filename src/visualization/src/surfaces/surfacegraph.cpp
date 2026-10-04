#include "core/performance.h"
#include "visualization/surfaces/surfacegraph.h"
#include "visualization/surfaces/surfacegraphmodifier.h"

#include <QtWidgets/qboxlayout.h>
#include <QtWidgets/qcheckbox.h>
#include <QtWidgets/qcombobox.h>
#include <QtWidgets/qradiobutton.h>
#include <QtWidgets/qgroupbox.h>
#include <QtWidgets/qlabel.h>
#include <QtWidgets/qcommandlinkbutton.h>
#include <QtGui/qpainter.h>
#include <QOpenGLContext>
#include <QtDataVisualization/qutils.h>
#include <QOffscreenSurface>
#include <QKeyEvent>
#include <QScrollArea>
#include <QWheelEvent>
#include <QHideEvent>
#include <QSignalBlocker>
#include <QJsonArray>
#include <QtMath>
#include <algorithm>
#include <cmath>
#include <functional>

using namespace Qt::StringLiterals;

// Coalesce camera/data changes into at most 60 native frames/s.
// Leave the Qt scene and buffer swap on their owning GUI thread.
class PacedSurface final : public Q3DSurface {
public:
    explicit PacedSurface(const QSurfaceFormat *format) : Q3DSurface(format) {}
    std::function<void()> prepare;
private:
    QElapsedTimer _clock;
    qint64 _last_frame=-16666667;
    bool _pending=false;
    void renderFrame() {
        _pending=false;
        if(!isExposed()) return;
        _last_frame=_clock.nsecsElapsed();
        if(prepare) prepare();
        QEvent update(QEvent::UpdateRequest);
        core::perf::Scope trace(core::perf::Stage::SurfaceRender);
        Q3DSurface::event(&update);
    }
protected:
    bool event(QEvent *event) override {
        if(event->type()==QEvent::UpdateRequest) {
            if(!_clock.isValid()) _clock.start();
            if(_pending) return true;
            const auto remaining=16666667-(_clock.nsecsElapsed()-_last_frame);
            if(remaining<=0) renderFrame();
            else {
                _pending=true;
                QTimer::singleShot(int((remaining+999999)/1000000),Qt::PreciseTimer,this,[this] { renderFrame(); });
            }
            return true;
        }
        return Q3DSurface::event(event);
    }
};

static QPixmap gradientBtoYPB_Pixmap() {
    QLinearGradient grBtoY(0, 0, 1, 100);
    grBtoY.setColorAt(1.f, Qt::black);
    grBtoY.setColorAt(0.67f, Qt::blue);
    grBtoY.setColorAt(0.33f, Qt::red);
    grBtoY.setColorAt(0.f, Qt::yellow);
    QPixmap pm(24, 100);
    QPainter pmp(&pm);
    pmp.setBrush(QBrush(grBtoY));
    pmp.setPen(Qt::NoPen);
    pmp.drawRect(0, 0, 24, 100);
    return pm;
}

static QPixmap gradientGtoRPB_Pixmap() {
    QLinearGradient grGtoR(0, 0, 1, 100);
    grGtoR.setColorAt(1.f, Qt::darkGreen);
    grGtoR.setColorAt(0.5f, Qt::yellow);
    grGtoR.setColorAt(0.2f, Qt::red);
    grGtoR.setColorAt(0.f, Qt::darkRed);
    QPixmap pm(24, 100);
    QPainter pmp(&pm);
    pmp.setBrush(QBrush(grGtoR));
    pmp.drawRect(0, 0, 24, 100);
    return pm;
}

static QPixmap highlightPixmap() {
    constexpr int height = 400;
    constexpr int width = 110;
    constexpr int border = 10;
    QLinearGradient gr(0, 0, 1, height - 2 * border);
    gr.setColorAt(1.f, Qt::black);
    gr.setColorAt(0.8f, Qt::darkGreen);
    gr.setColorAt(0.6f, Qt::green);
    gr.setColorAt(0.4f, Qt::yellow);
    gr.setColorAt(0.2f, Qt::red);
    gr.setColorAt(0.f, Qt::darkRed);

    QPixmap pmHighlight(width, height);
    pmHighlight.fill(Qt::transparent);
    QPainter pmpHighlight(&pmHighlight);
    pmpHighlight.setBrush(QBrush(gr));
    pmpHighlight.setPen(Qt::NoPen);
    pmpHighlight.drawRect(border, border, 35, height - 2 * border);
    pmpHighlight.setPen(Qt::black);
    int step = (height - 2 * border) / 5;
    for (int i = 0; i < 6; ++i) {
        int yPos = i * step + border;
        pmpHighlight.drawLine(border, yPos, 55, yPos);
        const int height = 550 - (i * 110);
        pmpHighlight.drawText(60, yPos + 2, QString::number(height) + u" m"_s);
    }
    return pmHighlight;
}


SurfaceGraph::SurfaceGraph(QWidget *parent) : QWidget(parent) {
    _camera_timer.setInterval(16);
    _camera_timer.setTimerType(Qt::PreciseTimer);
    connect(&_camera_timer, &QTimer::timeout, this, [this] { if(m_surfaceGraph) m_surfaceGraph->requestUpdate(); });
}

SurfaceGraph::~SurfaceGraph() {
    _stop_camera();
    if (!m_container) delete m_surfaceGraph;
}

bool SurfaceGraph::initialize(const QSize &minimumGraphSize, const QSize &maximumGraphSize) {
    if (_initialization_attempted) return _modifier != nullptr;
    _initialization_attempted = true;
    QOpenGLContext probe;
    QOffscreenSurface surface;
    if (!probe.create()) {
        auto *layout = new QVBoxLayout(this);
        layout->addWidget(new QLabel("3D visualization is unavailable: no graphics context.", this));
        return false;
    }
    surface.setFormat(probe.format());
    surface.create();
    if (!surface.isValid() || !probe.makeCurrent(&surface)) {
        auto *layout = new QVBoxLayout(this);
        layout->addWidget(new QLabel("3D visualization is unavailable: no graphics context.", this));
        return false;
    }
    probe.doneCurrent();
    auto format=qDefaultSurfaceFormat(true);
    format.setSwapInterval(1);
    m_surfaceGraph = new PacedSurface(&format);
    if (!m_surfaceGraph->hasContext()) {
        auto *layout = new QVBoxLayout(this);
        layout->addWidget(new QLabel("3D visualization is unavailable: no graphics context.", this));
        return false;
    }
    auto *hLayout = new QHBoxLayout(this);
    m_container = QWidget::createWindowContainer(m_surfaceGraph, this);
    Q_UNUSED(minimumGraphSize);
    Q_UNUSED(maximumGraphSize);
    m_container->setMinimumSize(240, 180);
    m_container->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_container->setFocusPolicy(Qt::StrongFocus);
    m_container->installEventFilter(this);
    m_surfaceGraph->installEventFilter(this);
    m_surfaceGraph->setObjectName("surfaceWindow");
    hLayout->setContentsMargins(0, 0, 0, 0);
    _splitter = new QSplitter(Qt::Horizontal, this);
    _splitter->setObjectName("surfaceSplitter");
    _splitter->setChildrenCollapsible(false);
    _splitter->setHandleWidth(8);
    _splitter->addWidget(m_container);
    auto *sideScroll = new QScrollArea(_splitter);
    sideScroll->setWidgetResizable(true);
    sideScroll->setFrameShape(QFrame::NoFrame);
    sideScroll->setMinimumWidth(200);
    auto *sidePanel = new QWidget(sideScroll);
    sidePanel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    sideScroll->setWidget(sidePanel);
    auto *vLayout = new QVBoxLayout(sidePanel);
    _splitter->addWidget(sideScroll);
    _splitter->setStretchFactor(0, 1);
    _splitter->setStretchFactor(1, 0);
    hLayout->addWidget(_splitter);
    set_side_panel_ratio(_side_panel_ratio);
    connect(_splitter, &QSplitter::splitterMoved, this, [this] {
        const auto sizes = _splitter->sizes();
        const auto total = sizes[0] + sizes[1];
        if (total > 0) {
            _side_panel_ratio = static_cast<float>(sizes[1]) / total;
            emit side_panel_resized(_side_panel_ratio);
        }
    });
    vLayout->setAlignment(Qt::AlignTop);

    auto *signalGroupBox = new QGroupBox(u"Signal Settings"_s);

    auto *pauseButton = new QCheckBox(this);
    pauseButton->setCheckState(_pause ? Qt::Unchecked : Qt::Checked);
    pauseButton->setText(u"Pause"_s);
    pauseButton->setChecked(false);

    auto *pauseVBox = new QVBoxLayout;
    pauseVBox->addWidget(pauseButton);
    signalGroupBox->setLayout(pauseVBox);

    // Create control widgets
    auto *modelGroupBox = new QGroupBox(u"Model"_s);

    auto *toggle3DModelRB = new QRadioButton(this);
    toggle3DModelRB->setText(u"3D"_s);
    toggle3DModelRB->setChecked(false);

    auto *modelVBox = new QVBoxLayout;
    modelVBox->addWidget(toggle3DModelRB);
    modelGroupBox->setLayout(modelVBox);

    auto *selectionGroupBox = new QGroupBox(u"Graph Selection Mode"_s);

    auto *modeNoneRB = new QRadioButton(this);
    modeNoneRB->setText(u"No selection"_s);
    modeNoneRB->setChecked(false);

    auto *modeItemRB = new QRadioButton(this);
    modeItemRB->setText(u"Item"_s);
    modeItemRB->setChecked(false);

    auto *modeSliceRowRB = new QRadioButton(this);
    modeSliceRowRB->setText(u"Time Slice"_s);
    modeSliceRowRB->setChecked(false);

    auto *modeSliceColumnRB = new QRadioButton(this);
    modeSliceColumnRB->setText(u"Frequency Slice"_s);
    modeSliceColumnRB->setChecked(false);

    auto *selectionVBox = new QVBoxLayout;
    selectionVBox->addWidget(modeNoneRB);
    selectionVBox->addWidget(modeItemRB);
    selectionVBox->addWidget(modeSliceRowRB);
    selectionVBox->addWidget(modeSliceColumnRB);
    selectionGroupBox->setLayout(selectionVBox);

    auto *axisGroupBox = new QGroupBox(u"Axis ranges"_s);

    auto *axisMinSliderX = new QSlider(Qt::Horizontal);
    axisMinSliderX->setMinimum(0);
    axisMinSliderX->setTickInterval(1);
    axisMinSliderX->setEnabled(true);
    auto *axisMaxSliderX = new QSlider(Qt::Horizontal);
    axisMaxSliderX->setMinimum(1);
    axisMaxSliderX->setTickInterval(1);
    axisMaxSliderX->setEnabled(true);

    auto *axisMinSliderZ = new QSlider(Qt::Horizontal);
    axisMinSliderZ->setMinimum(0);
    axisMinSliderZ->setTickInterval(1);
    axisMinSliderZ->setEnabled(true);
    auto *axisMaxSliderZ = new QSlider(Qt::Horizontal);
    axisMaxSliderZ->setMinimum(1);
    axisMaxSliderZ->setTickInterval(1);
    axisMaxSliderZ->setEnabled(true);

    auto *axisVBox = new QVBoxLayout;
    axisVBox->addWidget(new QLabel(u"Time range (x)"_s));
    axisVBox->addWidget(axisMinSliderX);
    axisVBox->addWidget(axisMaxSliderX);
    axisVBox->addWidget(new QLabel(u"Freq range (z)"_s));
    axisVBox->addWidget(axisMinSliderZ);
    axisVBox->addWidget(axisMaxSliderZ);
    axisGroupBox->setLayout(axisVBox);

    auto *colorGroupBox = new QGroupBox(u"Custom gradient"_s);

    QPixmap pixmap = gradientBtoYPB_Pixmap();
    auto *gradientBtoYPB = new QPushButton(this);
    gradientBtoYPB->setIcon(QIcon(pixmap));
    gradientBtoYPB->setIconSize(pixmap.size());

    pixmap = gradientGtoRPB_Pixmap();
    auto *gradientGtoRPB = new QPushButton(this);
    gradientGtoRPB->setIcon(QIcon(pixmap));
    gradientGtoRPB->setIconSize(pixmap.size());
    auto *colorHBox = new QHBoxLayout;
    colorHBox->addWidget(gradientBtoYPB);
    colorHBox->addWidget(gradientGtoRPB);
    colorGroupBox->setLayout(colorHBox);

    // Multiseries heightmap


    auto *labelSelectedItem = new QLabel(u"Nothing"_s);
    labelSelectedItem->setVisible(false);


    auto *label = new QLabel(this);
    label->setPixmap(highlightPixmap());


    // Common
    vLayout->addWidget(signalGroupBox);
    vLayout->addWidget(modelGroupBox);
    vLayout->addWidget(selectionGroupBox);
    vLayout->addWidget(axisGroupBox);

    // Sqrt Sin
    vLayout->addWidget(colorGroupBox);

    // Multiseries heightmap
    vLayout->addWidget(labelSelectedItem);

    // Create the controller
    _modifier = new SurfaceGraphModifier(m_surfaceGraph, labelSelectedItem, this);


    connect(pauseButton, &QCheckBox::checkStateChanged, this, [this](const Qt::CheckState state) {
        _pause = state==Qt::Checked;
        _need_update = true;
        if(!_pause && m_surfaceGraph) m_surfaceGraph->requestUpdate();
    });

    connect(toggle3DModelRB, &QRadioButton::toggled,
            _modifier, &SurfaceGraphModifier::enable3DModel);


    connect(modeNoneRB, &QRadioButton::toggled,
            _modifier, &SurfaceGraphModifier::toggleModeNone);
    connect(modeItemRB, &QRadioButton::toggled,
            _modifier, &SurfaceGraphModifier::toggleModeItem);
    connect(modeSliceRowRB, &QRadioButton::toggled,
            _modifier, &SurfaceGraphModifier::toggleModeSliceRow);
    connect(modeSliceColumnRB, &QRadioButton::toggled,
            _modifier, &SurfaceGraphModifier::toggleModeSliceColumn);

    connect(axisMinSliderX, &QSlider::valueChanged,
            _modifier, &SurfaceGraphModifier::adjustXMin);
    connect(axisMaxSliderX, &QSlider::valueChanged,
            _modifier, &SurfaceGraphModifier::adjustXMax);
    connect(axisMinSliderZ, &QSlider::valueChanged,
            _modifier, &SurfaceGraphModifier::adjustZMin);
    connect(axisMaxSliderZ, &QSlider::valueChanged,
            _modifier, &SurfaceGraphModifier::adjustZMax);

    connect(gradientBtoYPB, &QPushButton::pressed,
            _modifier, &SurfaceGraphModifier::setBlackToYellowGradient);
    connect(gradientGtoRPB, &QPushButton::pressed,
            _modifier, &SurfaceGraphModifier::setGreenToRedGradient);

    connect(toggle3DModelRB, &QRadioButton::toggled,
            colorGroupBox, &QGroupBox::setVisible);

    _modifier->setAxisMinSliderX(axisMinSliderX);
    _modifier->setAxisMaxSliderX(axisMaxSliderX);
    _modifier->setAxisMinSliderZ(axisMinSliderZ);
    _modifier->setAxisMaxSliderZ(axisMaxSliderZ);
    _modifier->setGreenToRedGradient();

    auto *camera = m_surfaceGraph->scene()->activeCamera();
    camera->setMaxZoomLevel(maximum_camera_zoom);
    _default_camera_state = camera_state();
    if (!_camera_state.isEmpty()) set_camera_state(_camera_state);
    _camera_state = camera_state();
    connect(camera, &Q3DCamera::xRotationChanged, this, &SurfaceGraph::_camera_changed);
    connect(camera, &Q3DCamera::yRotationChanged, this, &SurfaceGraph::_camera_changed);
    connect(camera, &Q3DCamera::zoomLevelChanged, this, &SurfaceGraph::_camera_changed);
    connect(camera, &Q3DCamera::targetChanged, this, &SurfaceGraph::_camera_changed);
    auto *resetButton = new QPushButton("Reset camera", this);
    resetButton->setObjectName("resetCamera");
    vLayout->addWidget(resetButton);
    connect(resetButton, &QPushButton::clicked, this, &SurfaceGraph::reset_camera);

    toggle3DModelRB->setChecked(true);
    modeItemRB->setChecked(true);

    static_cast<PacedSurface *>(m_surfaceGraph)->prepare=[this] {
        if(!_camera_keys.isEmpty()) _move_camera();
        if (!_pause && _need_update && _modifier) {
            _modifier->update(_spectrogram);
            _need_update=false;
        }
    };

    return true;
}

void SurfaceGraph::update(const std::vector<std::vector<float> > &spectrogram) {
    core::perf::Scope trace(core::perf::Stage::SurfaceData);
    if (!_modifier) return;
    _spectrogram = spectrogram;
    _need_update = true;
    if(isVisible() && !_pause) m_surfaceGraph->requestUpdate();
}

QJsonObject SurfaceGraph::camera_state() const {
    if (!m_surfaceGraph || !_modifier) return _camera_state;
    const auto *camera = m_surfaceGraph->scene()->activeCamera();
    const auto target = camera->target();
    return {{"horizontal", camera->xRotation()}, {"vertical", camera->yRotation()},
            {"zoom", camera->zoomLevel()}, {"target", QJsonArray{target.x(), target.y(), target.z()}}};
}

void SurfaceGraph::set_camera_state(const QJsonObject &state) {
    if (state.isEmpty()) return;
    auto number = [&](const char *key, float fallback, float min, float max) {
        const auto value = state.value(key).toDouble(fallback);
        return std::isfinite(value) ? std::clamp(static_cast<float>(value), min, max) : fallback;
    };
    const auto target = state.value("target").toArray();
    auto coordinate = [&](int i) {
        const auto value = i < target.size() ? target[i].toDouble() : 0.0;
        return std::isfinite(value) ? std::clamp(static_cast<float>(value), -1.0f, 1.0f) : 0.0f;
    };
    _camera_state = {{"horizontal",number("horizontal",45,-180,180)}, {"vertical",number("vertical",45,0,90)},
                     {"zoom",number("zoom",155,10,maximum_camera_zoom)}, {"target",QJsonArray{coordinate(0),coordinate(1),coordinate(2)}}};
    if (!m_surfaceGraph || !_modifier) return;
    auto *camera = m_surfaceGraph->scene()->activeCamera();
    const QSignalBlocker blocker(camera);
    camera->setCameraPosition(_camera_state["horizontal"].toDouble(), _camera_state["vertical"].toDouble(), _camera_state["zoom"].toDouble());
    camera->setTarget(QVector3D(coordinate(0),coordinate(1),coordinate(2)));
    _camera_state = camera_state();
}

bool SurfaceGraph::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::Wheel && m_surfaceGraph && _modifier) {
        auto *wheel = static_cast<QWheelEvent *>(event);
        const float step = wheel->pixelDelta().isNull() ? wheel->angleDelta().y() / 1200.0f
                                                       : wheel->pixelDelta().y() / 200.0f;
        auto *camera = m_surfaceGraph->scene()->activeCamera();
        camera->setZoomLevel(camera->zoomLevel() * std::exp(std::clamp(step, -2.0f, 2.0f)));
        wheel->accept();
        return true;
    }
    if (event->type() == QEvent::FocusOut || event->type() == QEvent::WindowDeactivate) _stop_camera();
    if (event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape) { _stop_camera(); return true; }
        // Preserve application shortcuts. Ctrl held alone is a movement key.
        if (key->modifiers() & (Qt::AltModifier | Qt::MetaModifier)) {
            _stop_camera();
            return QWidget::eventFilter(watched, event);
        }
        if (key->isAutoRepeat()) {
            for (const auto &values : _camera_controls)
                for (const auto &value : values.toArray())
                    if (CameraControls::matches(value.toObject(),*key)) return true;
            return QWidget::eventFilter(watched,event);
        }
        const int physical = CameraControls::scan(*key);
        const int identity = physical ? physical : CameraControls::logical(key->key()) + 0x10000;
        if (event->type() == QEvent::KeyRelease && _held_camera_keys.remove(identity)) {
            _camera_keys.clear();
            for (const auto &action : _held_camera_keys) _camera_keys.insert(action);
            if (_camera_keys.isEmpty()) _camera_timer.stop();
            return true;
        }
        bool handled = false;
        for (auto it = _camera_controls.constBegin(); it != _camera_controls.constEnd(); ++it) {
            for (const auto &value : it.value().toArray()) {
                if (!CameraControls::matches(value.toObject(), *key)) continue;
                handled = true;
                if (key->isAutoRepeat()) break;
                if (event->type() == QEvent::KeyRelease) _camera_keys.remove(it.key());
                else if (it.key() == "reset") reset_camera();
                else { _held_camera_keys.insert(identity,it.key()); _camera_keys.insert(it.key()); }
                break;
            }
        }
        if (handled) {
            if (_camera_keys.isEmpty()) _camera_timer.stop();
            else if (!_camera_timer.isActive()) { _camera_clock.start(); _camera_timer.start(); }
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void SurfaceGraph::_stop_camera() {
    _camera_keys.clear();
    _held_camera_keys.clear();
    _camera_timer.stop();
}

void SurfaceGraph::hideEvent(QHideEvent *event) {
    _stop_camera();
    QWidget::hideEvent(event);
}

void SurfaceGraph::_move_camera() {
    core::perf::Scope trace(core::perf::Stage::CameraMove);
    if (!m_surfaceGraph || !_modifier) { _stop_camera(); return; }
    auto *camera = m_surfaceGraph->scene()->activeCamera();
    const auto dt = std::min(_camera_clock.restart() / 1000.0f, 0.05f);
    const auto speed = (_camera_keys.contains("fast") ? 2.5f : 1.0f) * dt;
    const float forward = _camera_keys.contains("forward") - _camera_keys.contains("backward");
    const float side = _camera_keys.contains("right") - _camera_keys.contains("left");
    const float up = _camera_keys.contains("up") - _camera_keys.contains("down");
    const auto yaw = qDegreesToRadians(camera->xRotation());
    auto target = camera->target();
    float ratio = m_surfaceGraph->horizontalAspectRatio();
    if (ratio <= 0) {
        const float x_range = m_surfaceGraph->axisX()->max() - m_surfaceGraph->axisX()->min();
        const float z_range = m_surfaceGraph->axisZ()->max() - m_surfaceGraph->axisZ()->min();
        ratio = z_range > 0 ? x_range / z_range : 1.0f;
    }
    target += camera_translation(yaw,side,up,ratio,m_surfaceGraph->aspectRatio()) * speed;
    camera->setTarget(target);
    camera->setZoomLevel(camera->zoomLevel() * std::exp(forward * speed));
}

void SurfaceGraph::_camera_changed() {
    _camera_state = camera_state();
    emit camera_changed(_camera_state);
}

void SurfaceGraph::set_side_panel_ratio(float ratio) {
    _side_panel_ratio = std::isfinite(ratio) ? std::clamp(ratio, 0.01f, 0.99f) : 0.25f;
    if (_splitter) _splitter->setSizes({static_cast<int>(1000 * (1 - _side_panel_ratio)), static_cast<int>(1000 * _side_panel_ratio)});
}

void SurfaceGraph::set_camera_controls(const QJsonObject &controls) {
    _stop_camera();
    _camera_controls = CameraControls::normalized(controls);
}
void SurfaceGraph::reset_camera() {
    _stop_camera();
    if (_default_camera_state.isEmpty()) return;
    set_camera_state(_default_camera_state);
    _camera_changed();
}
QVector3D SurfaceGraph::camera_translation(float yaw, float side, float up, float horizontal_ratio, float aspect) {
    const float ratio = std::max(horizontal_ratio, 0.001f);
    const float x_scale = std::min(1.0f, ratio);
    const float z_scale = std::min(1.0f, 1.0f / ratio);
    QVector3D direction(std::cos(yaw) * side, up, -std::sin(yaw) * side);
    if (direction.lengthSquared() > 1.0f) direction.normalize();
    // Camera target coordinates are normalized independently on each axis.
    // Convert world-space motion into that coordinate system after rotation.
    return {direction.x() / x_scale, direction.y() * aspect, direction.z() / z_scale};
}

void SurfaceGraph::showEvent(QShowEvent *event) {
    QWidget::showEvent(event); if (_modifier) m_surfaceGraph->requestUpdate();
}
