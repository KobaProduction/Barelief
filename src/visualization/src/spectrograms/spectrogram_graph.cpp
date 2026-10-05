#include "visualization/ui_text.h"
#include "core/performance.h"
#include "visualization/spectrograms/spectrogram_graph.h"
#include "visualization/spectrograms/spectrogram_graph_modifier.h"

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
class PacedSpectrogram final : public Q3DSurface {
public:
    explicit PacedSpectrogram(const QSurfaceFormat *format) : Q3DSurface(format) {}
    std::function<void()> prepare;
    qreal fps() const {
        if (!_clock.isValid() || _clock.nsecsElapsed()-_last_render>1500000000) return 0;
        return _fps;
    }
    void stopRendering() {
        _stopped = true;
        prepare = {};
    }
private:
    QElapsedTimer _clock;
    qint64 _next_frame=0;
    bool _pending=false;
    bool _stopped=false;
    qint64 _fps_start=0, _last_render=0;
    quint64 _fps_frames=0;
    qreal _fps=0;
    void renderFrame() {
        _pending=false;
        if(_stopped || !isExposed()) return;
        const auto start = _clock.nsecsElapsed();
        if(prepare) prepare();
        QEvent update(QEvent::UpdateRequest);
        core::perf::Scope trace(core::perf::Stage::SpectrogramRender);
        Q3DSurface::event(&update);
        _last_render=_clock.nsecsElapsed(); ++_fps_frames;
        const auto elapsed=_last_render-_fps_start;
        if (elapsed>=1000000000) { _fps=_fps_frames*1e9/elapsed; _fps_frames=0; _fps_start=_last_render; }
        // A slow driver must leave time for input/paint events between frames.
        _next_frame = std::max(start + 16666667, _clock.nsecsElapsed() + 1000000);
    }
protected:
    void exposeEvent(QExposeEvent *) override {
        // Qt renders synchronously from its default expose handler. Route it
        // through the same scheduler as camera/data updates and count the frame.
        if (!_stopped && isExposed()) requestUpdate();
    }
    bool event(QEvent *event) override {
        if(event->type()==QEvent::UpdateRequest) {
            if (_stopped) return true;
            if(!_clock.isValid()) _clock.start();
            if(_pending) return true;
            const auto remaining=_next_frame-_clock.nsecsElapsed();
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
qreal SpectrogramGraph::display_fps() const {
    return m_spectrogram_graph && isVisible() ? static_cast<PacedSpectrogram *>(m_spectrogram_graph)->fps() : 0;
}

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


SpectrogramGraph::SpectrogramGraph(QWidget *parent) : QWidget(parent) {
    _camera_timer.setInterval(16);
    _camera_timer.setTimerType(Qt::PreciseTimer);
    connect(&_camera_timer, &QTimer::timeout, this, [this] { if(m_spectrogram_graph) m_spectrogram_graph->requestUpdate(); });
}

SpectrogramGraph::~SpectrogramGraph() {
    _stop_camera();
    // The container owns the native window and is destroyed by QWidget later,
    // after this object's members. Native teardown may dispatch pending events.
    // Detach the callback capturing this before those members are destroyed.
    if (m_spectrogram_graph) {
        static_cast<PacedSpectrogram *>(m_spectrogram_graph)->stopRendering();
        m_spectrogram_graph->removeEventFilter(this);
        m_spectrogram_graph->scene()->activeCamera()->disconnect(this);
    }
    if (m_container) m_container->removeEventFilter(this);
    // Destroy the owning container while this object's state is still alive.
    // QWidget base destruction otherwise happens after the members above.
    if (m_container) delete m_container;
    else delete m_spectrogram_graph;
    m_container = nullptr;
    m_spectrogram_graph = nullptr;
}

bool SpectrogramGraph::initialize(const QSize &minimumGraphSize, const QSize &maximumGraphSize) {
    if (_initialization_attempted) return _modifier != nullptr;
    _initialization_attempted = true;
    // These QPA plugins cannot host a native 3D window even when Mesa exposes GL.
    const auto platform = QGuiApplication::platformName();
    if (platform == "offscreen" || platform == "minimal") {
        auto *layout = new QVBoxLayout(this);
        layout->addWidget(ui::make<QLabel>("3D visualization requires a native graphical session.", this));
        return false;
    }
    QOpenGLContext probe;
    QOffscreenSurface gl_surface;
    if (!probe.create()) {
        auto *layout = new QVBoxLayout(this);
        layout->addWidget(ui::make<QLabel>("3D visualization is unavailable: no graphics context.", this));
        return false;
    }
    gl_surface.setFormat(probe.format());
    gl_surface.create();
    if (!gl_surface.isValid() || !probe.makeCurrent(&gl_surface)) {
        auto *layout = new QVBoxLayout(this);
        layout->addWidget(ui::make<QLabel>("3D visualization is unavailable: no graphics context.", this));
        return false;
    }
    probe.doneCurrent();
    auto format=qDefaultSurfaceFormat(true);
#if defined(__linux__)
    format.setVersion(2,0);
#endif
    format.setSwapInterval(1);
    m_spectrogram_graph = new PacedSpectrogram(&format);
    if (!m_spectrogram_graph->hasContext()) {
        auto *layout = new QVBoxLayout(this);
        layout->addWidget(ui::make<QLabel>("3D visualization is unavailable: no graphics context.", this));
        return false;
    }
    auto *hLayout = new QHBoxLayout(this);
    m_container = QWidget::createWindowContainer(m_spectrogram_graph, this);
    m_container->setObjectName("spectrogramContainer");
    Q_UNUSED(minimumGraphSize);
    Q_UNUSED(maximumGraphSize);
    m_container->setMinimumSize(240, 180);
    m_container->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_container->setFocusPolicy(Qt::StrongFocus);
    m_container->installEventFilter(this);
    m_spectrogram_graph->installEventFilter(this);
    m_spectrogram_graph->setObjectName("spectrogramWindow");
    hLayout->setContentsMargins(0, 0, 0, 0);
    _splitter = new QSplitter(Qt::Horizontal, this);
    _splitter->setObjectName("spectrogramSplitter");
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

    auto *signalGroupBox = ui::make<QGroupBox>("Signal Settings", this);

    auto *pauseButton = new QCheckBox(this);
    pauseButton->setCheckState(_pause ? Qt::Unchecked : Qt::Checked);
    ui::bind(pauseButton,"text","Pause");
    pauseButton->setChecked(false);

    auto *pauseVBox = new QVBoxLayout;
    pauseVBox->addWidget(pauseButton);
    auto *information=new QLabel(signalGroupBox);
    information->setWordWrap(true); information->setObjectName("spectrogramGraphInfo");
    pauseVBox->addWidget(information);
    auto *information_timer=new QTimer(this); information_timer->setInterval(500);
    connect(information_timer,&QTimer::timeout,this,[this,information] {
        if (isVisible()) information->setText(ui::text("History: %1 frames · %2 bands").arg(_spectrogram.empty() ? 0 : _spectrogram.back().size()).arg(_spectrogram.size()));
    }); information_timer->start();
    signalGroupBox->setLayout(pauseVBox);

    // Create control widgets
    auto *modelGroupBox = ui::make<QGroupBox>("Model", this);

    auto *toggle3DModelRB = new QRadioButton(this);
    ui::bind(toggle3DModelRB,"text","3D");
    toggle3DModelRB->setChecked(false);

    auto *modelVBox = new QVBoxLayout;
    modelVBox->addWidget(toggle3DModelRB);
    modelGroupBox->setLayout(modelVBox);

    auto *selectionGroupBox = ui::make<QGroupBox>("Graph Selection Mode", this);

    auto *modeNoneRB = new QRadioButton(this);
    ui::bind(modeNoneRB,"text","No selection");
    modeNoneRB->setChecked(false);

    auto *modeItemRB = new QRadioButton(this);
    ui::bind(modeItemRB,"text","Item");
    modeItemRB->setChecked(false);

    auto *modeSliceRowRB = new QRadioButton(this);
    ui::bind(modeSliceRowRB,"text","Time Slice");
    modeSliceRowRB->setChecked(false);

    auto *modeSliceColumnRB = new QRadioButton(this);
    ui::bind(modeSliceColumnRB,"text","Frequency Slice");
    modeSliceColumnRB->setChecked(false);

    auto *selectionVBox = new QVBoxLayout;
    selectionVBox->addWidget(modeNoneRB);
    selectionVBox->addWidget(modeItemRB);
    selectionVBox->addWidget(modeSliceRowRB);
    selectionVBox->addWidget(modeSliceColumnRB);
    selectionGroupBox->setLayout(selectionVBox);

    auto *axisGroupBox = ui::make<QGroupBox>("Axis ranges", this);

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
    axisVBox->addWidget(ui::make<QLabel>("Time range (x)", this));
    axisVBox->addWidget(axisMinSliderX);
    axisVBox->addWidget(axisMaxSliderX);
    axisVBox->addWidget(ui::make<QLabel>("Freq range (z)", this));
    axisVBox->addWidget(axisMinSliderZ);
    axisVBox->addWidget(axisMaxSliderZ);
    axisGroupBox->setLayout(axisVBox);

    auto *colorGroupBox = ui::make<QGroupBox>("Custom gradient", this);

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


    auto *labelSelectedItem = ui::make<QLabel>("Nothing", this);
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
    _modifier = new SpectrogramGraphModifier(m_spectrogram_graph, labelSelectedItem, this);


    connect(pauseButton, &QCheckBox::checkStateChanged, this, [this](const Qt::CheckState state) {
        _pause = state==Qt::Checked;
        _need_update = true;
        if(!_pause && m_spectrogram_graph) m_spectrogram_graph->requestUpdate();
    });

    connect(toggle3DModelRB, &QRadioButton::toggled,
            _modifier, &SpectrogramGraphModifier::enable3DModel);


    connect(modeNoneRB, &QRadioButton::toggled,
            _modifier, &SpectrogramGraphModifier::toggleModeNone);
    connect(modeItemRB, &QRadioButton::toggled,
            _modifier, &SpectrogramGraphModifier::toggleModeItem);
    connect(modeSliceRowRB, &QRadioButton::toggled,
            _modifier, &SpectrogramGraphModifier::toggleModeSliceRow);
    connect(modeSliceColumnRB, &QRadioButton::toggled,
            _modifier, &SpectrogramGraphModifier::toggleModeSliceColumn);

    connect(axisMinSliderX, &QSlider::valueChanged,
            _modifier, &SpectrogramGraphModifier::adjustXMin);
    connect(axisMaxSliderX, &QSlider::valueChanged,
            _modifier, &SpectrogramGraphModifier::adjustXMax);
    connect(axisMinSliderZ, &QSlider::valueChanged,
            _modifier, &SpectrogramGraphModifier::adjustZMin);
    connect(axisMaxSliderZ, &QSlider::valueChanged,
            _modifier, &SpectrogramGraphModifier::adjustZMax);

    connect(gradientBtoYPB, &QPushButton::pressed,
            _modifier, &SpectrogramGraphModifier::setBlackToYellowGradient);
    connect(gradientGtoRPB, &QPushButton::pressed,
            _modifier, &SpectrogramGraphModifier::setGreenToRedGradient);

    connect(toggle3DModelRB, &QRadioButton::toggled,
            colorGroupBox, &QGroupBox::setVisible);

    _modifier->setAxisMinSliderX(axisMinSliderX);
    _modifier->setAxisMaxSliderX(axisMaxSliderX);
    _modifier->setAxisMinSliderZ(axisMinSliderZ);
    _modifier->setAxisMaxSliderZ(axisMaxSliderZ);
    _modifier->setGreenToRedGradient();

    auto *camera = m_spectrogram_graph->scene()->activeCamera();
    camera->setMaxZoomLevel(maximum_camera_zoom);
    _default_camera_state = camera_state();
    if (!_camera_state.isEmpty()) set_camera_state(_camera_state);
    _camera_state = camera_state();
    connect(camera, &Q3DCamera::xRotationChanged, this, &SpectrogramGraph::_camera_changed);
    connect(camera, &Q3DCamera::yRotationChanged, this, &SpectrogramGraph::_camera_changed);
    connect(camera, &Q3DCamera::zoomLevelChanged, this, &SpectrogramGraph::_camera_changed);
    connect(camera, &Q3DCamera::targetChanged, this, &SpectrogramGraph::_camera_changed);
    auto *resetButton = ui::make<QPushButton>("Reset camera", this);
    resetButton->setObjectName("resetCamera");
    vLayout->addWidget(resetButton);
    connect(resetButton, &QPushButton::clicked, this, &SpectrogramGraph::reset_camera);

    toggle3DModelRB->setChecked(true);
    modeItemRB->setChecked(true);

    static_cast<PacedSpectrogram *>(m_spectrogram_graph)->prepare=[this] {
        if(!_camera_keys.isEmpty()) _move_camera();
        if (!_pause && _need_update && _modifier) {
            _modifier->update(_spectrogram);
            _need_update=false;
        }
#if defined(_WIN32)
        // Keep native presentation progressing without repainting every control
        // in the Spectrogram side panel on each 3D frame.
        if (m_container) m_container->update();
#endif
    };

    return true;
}

void SpectrogramGraph::update(const std::vector<std::vector<float> > &spectrogram) {
    core::perf::Scope trace(core::perf::Stage::SpectrogramData);
    if (!_modifier) return;
    if (_spectrogram == spectrogram) return;
    _spectrogram = spectrogram;
    _need_update = true;
    if(isVisible() && !_pause) m_spectrogram_graph->requestUpdate();
}

QJsonObject SpectrogramGraph::camera_state() const {
    if (!m_spectrogram_graph || !_modifier) return _camera_state;
    const auto *camera = m_spectrogram_graph->scene()->activeCamera();
    const auto target = camera->target();
    return {{"horizontal", camera->xRotation()}, {"vertical", camera->yRotation()},
            {"zoom", camera->zoomLevel()}, {"target", QJsonArray{target.x(), target.y(), target.z()}}};
}

void SpectrogramGraph::set_camera_state(const QJsonObject &state) {
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
    if (!m_spectrogram_graph || !_modifier) return;
    auto *camera = m_spectrogram_graph->scene()->activeCamera();
    const QSignalBlocker blocker(camera);
    camera->setCameraPosition(_camera_state["horizontal"].toDouble(), _camera_state["vertical"].toDouble(), _camera_state["zoom"].toDouble());
    camera->setTarget(QVector3D(coordinate(0),coordinate(1),coordinate(2)));
    _camera_state = camera_state();
}

bool SpectrogramGraph::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::Wheel && m_spectrogram_graph && _modifier) {
        auto *wheel = static_cast<QWheelEvent *>(event);
        const float step = wheel->pixelDelta().isNull() ? wheel->angleDelta().y() / 1200.0f
                                                       : wheel->pixelDelta().y() / 200.0f;
        auto *camera = m_spectrogram_graph->scene()->activeCamera();
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

void SpectrogramGraph::_stop_camera() {
    _camera_keys.clear();
    _held_camera_keys.clear();
    _camera_timer.stop();
}

void SpectrogramGraph::hideEvent(QHideEvent *event) {
    _stop_camera();
    QWidget::hideEvent(event);
}

void SpectrogramGraph::_move_camera() {
    core::perf::Scope trace(core::perf::Stage::CameraMove);
    if (!m_spectrogram_graph || !_modifier) { _stop_camera(); return; }
    auto *camera = m_spectrogram_graph->scene()->activeCamera();
    const auto dt = std::min(_camera_clock.restart() / 1000.0f, 0.05f);
    const auto speed = (_camera_keys.contains("fast") ? 2.5f : 1.0f) * dt;
    const float forward = _camera_keys.contains("forward") - _camera_keys.contains("backward");
    const float side = _camera_keys.contains("right") - _camera_keys.contains("left");
    const float up = _camera_keys.contains("up") - _camera_keys.contains("down");
    const auto yaw = qDegreesToRadians(camera->xRotation());
    auto target = camera->target();
    float ratio = m_spectrogram_graph->horizontalAspectRatio();
    if (ratio <= 0) {
        const float x_range = m_spectrogram_graph->axisX()->max() - m_spectrogram_graph->axisX()->min();
        const float z_range = m_spectrogram_graph->axisZ()->max() - m_spectrogram_graph->axisZ()->min();
        ratio = z_range > 0 ? x_range / z_range : 1.0f;
    }
    target += camera_translation(yaw,side,up,ratio,m_spectrogram_graph->aspectRatio()) * speed;
    camera->setTarget(target);
    camera->setZoomLevel(camera->zoomLevel() * std::exp(forward * speed));
}

void SpectrogramGraph::_camera_changed() {
    _camera_state = camera_state();
    emit camera_changed(_camera_state);
}

void SpectrogramGraph::set_side_panel_ratio(float ratio) {
    _side_panel_ratio = std::isfinite(ratio) ? std::clamp(ratio, 0.01f, 0.99f) : 0.25f;
    if (_splitter) _splitter->setSizes({static_cast<int>(1000 * (1 - _side_panel_ratio)), static_cast<int>(1000 * _side_panel_ratio)});
}

void SpectrogramGraph::set_camera_controls(const QJsonObject &controls) {
    _stop_camera();
    _camera_controls = CameraControls::normalized(controls);
}
void SpectrogramGraph::reset_camera() {
    _stop_camera();
    if (_default_camera_state.isEmpty()) return;
    set_camera_state(_default_camera_state);
    _camera_changed();
}
QVector3D SpectrogramGraph::camera_translation(float yaw, float side, float up, float horizontal_ratio, float aspect) {
    const float ratio = std::max(horizontal_ratio, 0.001f);
    const float x_scale = std::min(1.0f, ratio);
    const float z_scale = std::min(1.0f, 1.0f / ratio);
    QVector3D direction(std::cos(yaw) * side, up, -std::sin(yaw) * side);
    if (direction.lengthSquared() > 1.0f) direction.normalize();
    // Camera target coordinates are normalized independently on each axis.
    // Convert world-space motion into that coordinate system after rotation.
    return {direction.x() / x_scale, direction.y() * aspect, direction.z() / z_scale};
}

void SpectrogramGraph::showEvent(QShowEvent *event) {
    QWidget::showEvent(event); if (_modifier) m_spectrogram_graph->requestUpdate();
}
