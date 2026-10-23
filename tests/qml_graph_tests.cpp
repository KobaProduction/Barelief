#include <core_app/plot_composition.h>
#include <core_app/graph_panel.h>
#include <barelief/charts/line.h>
#include <barelief/numeric_image_view.h>
#include <barelief/numeric_metrics_view.h>
#include <barelief/numeric_pie_view.h>
#include <barelief/quick/quick_ui.h>
#include <barelief/quick2d/vector_plot_item.h>
#include <barelief/quick3d/surface_overlay_item.h>
#include <barelief/quick3d/surface_overlay_commands.h>
#include <QQmlComponent>
#include <QPainter>
#include <barelief/spectrograms/spectrogram_graph.h>
#include <QApplication>
#include <QQuickItem>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <QSignalSpy>
#include <QPointer>
#include <QJsonArray>
#include <QDir>
#include <cstdio>
#include <stdexcept>
#include <memory>
#include <limits>

namespace {
class SchedulingChart final : public LineChartView {};
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool wait(const std::function<bool()>& ready){QElapsedTimer time;time.start();while(!ready() && time.elapsed()<3000){QCoreApplication::processEvents();QThread::msleep(2);}return ready();}
template<class Host> void ready(Host* view){require(view && wait([&]{return view->status()==QQuickWidget::Ready;}),"QML scene failed to load");require(view->property("qmlErrors").toStringList().isEmpty(),"QML error telemetry nonempty");}
QQuickItem* item(QQuickItem* root,const QString& name){if(!root)return nullptr;if(root->objectName()==name)return root;for(const auto children=root->childItems();auto* child:children)if(auto* found=item(child,name))return found;return nullptr;}
}
namespace {
class QmlGraphScenario {

    int argc;
    QDir artifacts;
    std::unique_ptr<GraphPanel> vectorStorage, matrixStorage, surfaceStorage;
    std::unique_ptr<QQuickWidget> overlayHostStorage;
    std::unique_ptr<SurfaceOverlayCommands> overlayListStorage;
    std::unique_ptr<GraphPanel> metricsStorage;
    QQuickWidget *controls = nullptr;
    AbstractChartView *chart = nullptr;
    presentation::NumericRenderTarget target;
    QObject *axisEditors = nullptr;
    QQuickItem *channels = nullptr;
    QQuickItem *hiddenPie = nullptr;
    QVariant hiddenPaints;
    QQuickItem *visibleVector = nullptr;
    std::vector<float> samples;
    SpectrogramGraph *surfaceView = nullptr;
    QQuickWidget *surfaceHost = nullptr;
    SurfaceOverlayItem *surfaceOverlay = nullptr;
    NumericMetricsView *values = nullptr;
    GraphPanel &vector() { return *vectorStorage; }
    GraphPanel &matrix() { return *matrixStorage; }
    GraphPanel &surface() { return *surfaceStorage; }
    GraphPanel &metrics() { return *metricsStorage; }
    QQuickWidget &overlayHost() { return *overlayHostStorage; }
    SurfaceOverlayCommands &overlayList() { return *overlayListStorage; }
    std::unique_ptr<QSignalSpy> settingsNotificationsStorage;
    QSignalSpy &settingsNotifications() { return *settingsNotificationsStorage; }
    std::unique_ptr<QSignalSpy> diagnosticNotificationsStorage;
    QSignalSpy &diagnosticNotifications() { return *diagnosticNotificationsStorage; }
    std::unique_ptr<QSignalSpy> informationNotificationsStorage;
    QSignalSpy &informationNotifications() { return *informationNotificationsStorage; }
    std::unique_ptr<QSignalSpy> informationTicksStorage;
    QSignalSpy &informationTicks() { return *informationTicksStorage; }
    std::unique_ptr<QSignalSpy> unchangedPublicationsStorage;
    QSignalSpy &unchangedPublications() { return *unchangedPublicationsStorage; }
    std::unique_ptr<QSignalSpy> stableAxesStorage;
    QSignalSpy &stableAxes() { return *stableAxesStorage; }
    std::unique_ptr<QSignalSpy> burstPublicationsStorage;
    QSignalSpy &burstPublications() { return *burstPublicationsStorage; }
    std::unique_ptr<QSignalSpy> resumedPublicationsStorage;
    QSignalSpy &resumedPublications() { return *resumedPublicationsStorage; }
    std::unique_ptr<QSignalSpy> writesStorage;
    QSignalSpy &writes() { return *writesStorage; }
    std::unique_ptr<QSignalSpy> matrixWritesStorage;
    QSignalSpy &matrixWrites() { return *matrixWritesStorage; }
    std::unique_ptr<QSignalSpy> surfaceWritesStorage;
    QSignalSpy &surfaceWrites() { return *surfaceWritesStorage; }
    std::unique_ptr<QSignalSpy> overlayCommandWritesStorage;
    QSignalSpy &overlayCommandWrites() { return *overlayCommandWritesStorage; }
    std::unique_ptr<QSignalSpy> cameraPanelNotificationsStorage;
    QSignalSpy &cameraPanelNotifications() { return *cameraPanelNotificationsStorage; }
    std::unique_ptr<QSignalSpy> snapshotNotificationsStorage;
    QSignalSpy &snapshotNotifications() { return *snapshotNotificationsStorage; }
    std::unique_ptr<QSignalSpy> overlayNotificationsStorage;
    QSignalSpy &overlayNotifications() { return *overlayNotificationsStorage; }
    std::unique_ptr<QSignalSpy> metricsResetStorage;
    QSignalSpy &metricsReset() { return *metricsResetStorage; }
    std::unique_ptr<QSignalSpy> metricsPublicationsStorage;
    QSignalSpy &metricsPublications() { return *metricsPublicationsStorage; }
    template <class Host> void capture(Host *view, const QString &name) {
        if (argc != 2)
            return;
        require(artifacts.mkpath("."), "artifact directory");
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 100) {
            QCoreApplication::processEvents();
            QThread::msleep(2);
        }
        require(view->grabFramebuffer().save(artifacts.filePath(name + ".png")), "save own QML framebuffer");
    }

    static void noFocus(QWidget &view) {
        view.setAttribute(Qt::WA_ShowWithoutActivating);
        view.setWindowFlag(Qt::WindowDoesNotAcceptFocus);
    }

    qulonglong preparedCount() const {
        return visibleVector->property("paintCount").toULongLong() +
               visibleVector->property("geometryUpdateCount").toULongLong();
    }

    QImage overlayPixels() {
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 80) {
            QCoreApplication::processEvents();
            QThread::msleep(2);
        }
        return overlayHost().grabFramebuffer();
    }

    static QColor overlayColor(const QImage &image, int x, int y) {
        return image.pixelColor(qRound(x * image.width() / 180.), qRound(y * image.height() / 160.));
    }

    void setupVector() {

        auto *schedulingChart = new SchedulingChart;
        vectorStorage = std::make_unique<GraphPanel>(
            "vector", schedulingChart, QJsonObject{{"kind", "FFTView"}}, nullptr, nullptr,
            presentation::plot_capabilities(presentation::PlotDescriptor{{"line", "points", "bars", "pie"}, {}, true}));
        noFocus(vector());
        vector().resize(900, 500);
        vector().show();
        controls = vector().findChild<QQuickWidget *>("numericGraphSettings");
        ready(controls);
        chart = vector().active_chart();
        ready(chart);
    }

    void diagnosticNotificationsCase() {
        // Producer diagnostics must not invalidate the entire settings model:
        // its mode-dependent Repeater models describe configuration, not data.
        settingsNotificationsStorage = std::make_unique<QSignalSpy>(&vector(), &GraphPanel::state_changed);
        diagnosticNotificationsStorage = std::make_unique<QSignalSpy>(&vector(), &GraphPanel::diagnosticChanged);
        informationNotificationsStorage = std::make_unique<QSignalSpy>(&vector(), &GraphPanel::informationChanged);
        target = vector().render_target(true);
        require(target.exact_label && target.exact_label->caption, "settings caption port missing");
        for (int i = 0; i < 8; ++i)
            target.exact_label->caption("", {}, false, false);
        require(settingsNotifications().isEmpty() && diagnosticNotifications().isEmpty(),
                "repeated empty captions invalidated settings");
        target.exact_label->caption("Frames: %1", {"12"}, false, false);
        require(diagnosticNotifications().size() == 1 && settingsNotifications().isEmpty(),
                "changed diagnostic notified unrelated settings");
        auto *exactValue = controls->rootObject()->findChild<QObject *>("numericExactValue");
        require(exactValue && exactValue->property("text").toString() == vector().diagnostic(),
                "dedicated diagnostic notification did not update QML text");
        for (int i = 0; i < 8; ++i)
            target.exact_label->caption("Frames: %1", {"12"}, false, false);
        require(diagnosticNotifications().size() == 1 && settingsNotifications().isEmpty(),
                "identical rendered diagnostic republished settings");
        target.exact_label->caption("", {}, false, false);
        require(diagnosticNotifications().size() == 2 && exactValue->property("text").toString().isEmpty(),
                "diagnostic clearing did not notify its consumer");
    }

    void informationNotificationsCase() {
        QTimer *informationTimer = nullptr;
        for (const auto timers = vector().findChildren<QTimer *>(QString{}, Qt::FindDirectChildrenOnly); auto *timer : timers)
            if (timer->interval() == 500)
                informationTimer = timer;
        require(informationTimer, "information cadence timer missing");
        informationTicksStorage = std::make_unique<QSignalSpy>(informationTimer, &QTimer::timeout);
        require(wait([&] { return !informationTicks().isEmpty(); }), "information timer never ran");
        require(!vector().information().isEmpty() && settingsNotifications().isEmpty(),
                "information update invalidated unrelated settings");
        informationNotifications().clear();
        informationTicks().clear();
        require(wait([&] { return !informationTicks().isEmpty(); }), "second information timer tick missing");
        require(informationNotifications().isEmpty() && settingsNotifications().isEmpty(),
                "unchanged information timer text was republished");
        target.exact_label->caption("Frames: %1", {"12"}, false, false);
        const auto originalLanguage = ui::Language::instance().code();
        ui::Language::instance().set(originalLanguage == "ru" ? "en" : "ru");
        require(vector().diagnostic() == ui::text("Frames: %1").arg("12") &&
                    vector().information() == ui::text("Values: %1 · Peak: %2").arg(qulonglong(0)).arg(0., 0, 'g', 4),
                "language refresh left diagnostic or information in the old language");
        ui::Language::instance().set(originalLanguage);
        target.exact_label->caption("", {}, false, false);
        settingsNotifications().clear();
        // Real preference changes retain the configuration notifier.
        vector().set_render_fps(90);
        require(settingsNotifications().size() == 1, "settings change lost its notifier");
        vector().set_render_fps(60);
    }

    void vectorProjection() {
        chart->set_auto_resizing(false);
        chart->set_range(-1, 1);
        samples.assign(1000000, 0);
        samples[345678] = .9f;
        samples[345679] = -.8f;
        chart->update(samples);
        require(wait([&] { return chart->projected_point_count() > 0; }), "vector projection missing");
        require(chart->value_count() == 1000000 && chart->projected_point_count() <= 4098,
                "QML vector budget not bounded");
        const auto points = chart->projected_points();
        require(std::any_of(points.begin(), points.end(), [](auto p) { return p.y() == .9f; }) &&
                    std::any_of(points.begin(), points.end(), [](auto p) { return p.y() == -.8f; }),
                "display decimation lost peaks");
        capture(chart, "line");
        axisEditors = controls->rootObject()->findChild<QObject *>("cartesianAxisEditors");
        require(axisEditors && axisEditors->property("count").toInt() == 2, "Cartesian axis controls missing");
        channels = item(controls->rootObject(), "numericPlaneSelector");
        vector().set_numeric_plane_count(1, 0);
        QCoreApplication::processEvents();
        require(channels && !channels->isVisible(), "single-channel vector shows selector");
        vector().set_numeric_plane_count(2, 0);
        QCoreApplication::processEvents();
        require(channels->isVisible(), "multichannel vector lost selector");
        vector().selectPlane(1);
        require(vector().selected_plane() == 1, "source channel selection lost");
        vector().changePreference("line_width", 4.);
        require(wait([&] { return chart->presentation_model()->state().value("lineWidth").toDouble() == 4; }),
                "line width not applied");
        require(item(controls->rootObject(), "line_width") && !item(controls->rootObject(), "point_size"),
                "line controls contain another mode");
        chart->set_auto_resizing(false);
        chart->set_range(-1, 1);
    }

    void unchangedVectorIngress() {
        hiddenPie = item(chart->rootObject(), "numericPiePlot");
        require(hiddenPie && !hiddenPie->isVisible(), "hidden pie renderer missing");
        hiddenPaints = hiddenPie->property("paintCount");
        require(hiddenPaints.isValid(), "native paint counter missing");
        // Preference/layout changes now share the completion-driven scheduler.
        // Let those already requested frames finish before testing idle ingress.
        QElapsedTimer settle;
        settle.start();
        while (settle.elapsed() < 150) {
            QCoreApplication::processEvents();
            QThread::msleep(2);
        }
        unchangedPublicationsStorage =
            std::make_unique<QSignalSpy>(chart->presentation_model(), &QuickPlotModel::changed);
        stableAxesStorage = std::make_unique<QSignalSpy>(chart->presentation_model(), &QuickPlotModel::axesChanged);
        for (int i = 0; i < 8; ++i)
            chart->update(samples);
        QElapsedTimer unchangedWindow;
        unchangedWindow.start();
        while (unchangedWindow.elapsed() < 80) {
            QCoreApplication::processEvents();
            QThread::msleep(2);
        }
        require(unchangedPublications().isEmpty(), "unchanged vector unnecessarily republished geometry");
        visibleVector = item(chart->rootObject(), "numericVectorPlot");
        require(visibleVector, "native vector missing");
        require(visibleVector->property("sourceModel").value<QObject *>() == chart->presentation_model() &&
                    chart->presentation_model()->hasTypedVector() &&
                    !chart->presentation_model()->state().contains("points"),
                "production vector still crosses QML as point maps");

        const auto visiblePaints = preparedCount();
        samples[100] = .1f;
        chart->update(samples);
        require(wait([&] { return !unchangedPublications().isEmpty(); }), "changed vector did not publish");
        require(wait([&] { return preparedCount() > visiblePaints; }), "updated vector geometry was not consumed");
        require(stableAxes().isEmpty(), "data-only vector update rebuilt unchanged axis delegates");
        require(hiddenPie->property("paintCount") == hiddenPaints, "vector data painted hidden pie renderer");
    }

    void extremeVectorGeometry() {
        if (visibleVector->property("geometryUpdateCount").toULongLong()) {
            const auto initial = preparedCount();
            const auto savedPoints = chart->presentation_model()->vectorPoints();
            const auto extreme = std::numeric_limits<double>::max();
            chart->presentation_model()->publishVector(
                chart->presentation_model()->state(),
                QPolygonF{{-extreme, .2}, {extreme, .8}, {.5, std::numeric_limits<double>::quiet_NaN()}});
            require(wait([&] { return preparedCount() > initial; }),
                    "extreme manual-range series did not reach geometry renderer");
            require(visibleVector->property("geometryVerticesFinite").toBool() &&
                        visibleVector->property("geometryVertexCount").toInt() > 0,
                    "extreme series produced invalid GPU vertices or lost crossing segment");
            const auto crossing = chart->grabFramebuffer();
            const auto center = visibleVector->mapToScene({visibleVector->width() * .3, visibleVector->height() * .5});
            const auto accent = visibleVector->property("colors").toMap().value("highlight").value<QColor>();
            const int px = qRound(center.x() * crossing.width() / chart->width()),
                      py = qRound(center.y() * crossing.height() / chart->height());
            bool seriesAtMidpoint = false;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx) {
                    const auto c = crossing.pixelColor(px + dx, py + dy);
                    seriesAtMidpoint |= std::abs(c.red() - accent.red()) < 8 &&
                                        std::abs(c.green() - accent.green()) < 8 &&
                                        std::abs(c.blue() - accent.blue()) < 8;
                }
            require(seriesAtMidpoint, "extreme crossing segment was discarded or interpolated at the wrong height");
            chart->presentation_model()->publishVector(chart->presentation_model()->state(), savedPoints);
        }
    }

    void vectorBurst() {
        burstPublicationsStorage = std::make_unique<QSignalSpy>(chart->presentation_model(), &QuickPlotModel::changed);
        std::vector<float> burstSamples(256, 0);
        for (int i = 1; i <= 8; ++i) {
            burstSamples[150] = float(i) / 10;
            chart->update(burstSamples);
        }
        require(wait([&] { return !burstPublications().isEmpty(); }) && burstPublications().size() <= 2,
                "changed delivery burst was not coalesced within the cadence budget");
        require(wait([&] {
                    const auto latest = chart->projected_points();
                    return std::any_of(latest.begin(), latest.end(), [](auto p) { return p.y() == .8f; });
                }),
                "coalescing discarded latest vector data");
        require(burstPublications().size() <= 2, "delivery burst exceeded cadence budget");
        require(std::any_of(chart->presentation_model()->vectorPoints().cbegin(),
                            chart->presentation_model()->vectorPoints().cend(),
                            [](const QPointF &p) { return std::abs(p.y() - (1 - double(.8f)) / 2) < 1e-14; }),
                "typed handoff lost latest burst geometry or precision");
    }

    void pendingVectorCadence() {
        // Pending data must survive a cadence change without another ingress
        // event. The deliberately slow initial policy avoids an 8ms throughput
        // assertion tied to platform timer resolution.
        chart->set_render_fps(1);
        chart->set_range(-1, 1);
        resumedPublicationsStorage =
            std::make_unique<QSignalSpy>(chart->presentation_model(), &QuickPlotModel::changed);
        std::vector<float> pendingSamples(8, .25f);
        chart->update(pendingSamples);
        pendingSamples[3] = .75f;
        chart->update(pendingSamples);
        require(resumedPublications().isEmpty(), "pending vector ignored newly configured cadence");
        chart->set_render_fps(120);
        require(wait([&] {
                    const auto latest = chart->projected_points();
                    return latest.size() == 8 && latest[3].y() == .75f;
                }),
                "cadence change stranded the latest pending vector");
        require(resumedPublications().size() == 1, "cadence change duplicated pending vector publication");
        chart->set_render_visible(false);
        resumedPublications().clear();
        pendingSamples[3] = .5f;
        chart->update(pendingSamples);
        QCoreApplication::processEvents();
        require(resumedPublications().isEmpty(), "hidden vector published geometry");
        chart->set_render_visible(true);
        require(chart->projected_points().size() == 8 && chart->projected_points().at(3).y() == .5f,
                "showing vector failed to publish its newest supplied state");
        chart->set_render_fps(5);
        chart->set_range(-1, 1);
        pendingSamples[3] = .6f;
        chart->update(pendingSamples);
        chart->set_range(-2, 2);
        pendingSamples[3] = .7f;
        chart->update(pendingSamples);
        require(wait([&] {
                    const auto latest = chart->projected_points();
                    return latest.size() == 8 && latest[3].y() == .7f;
                }),
                "metadata refresh stranded the latest vector behind frame completion");
        require(chart->presentation_model()->state().value("baseline").toDouble() == .5,
                "coalesced metadata/data snapshot used inconsistent amplitude bounds");
        chart->set_range(-1, 1);
        chart->set_render_fps(60);
        chart->update(samples);
    }

    void vectorModesAndBins() {
        // FPS is sampled from scene rendering even with a QuickWidget host.
        require(wait([&] {
                    samples[200] = samples[200] == .2f ? .3f : .2f;
                    chart->update(samples);
                    return vector().display_fps() > 0;
                }),
                "actual scene FPS never sampled");
        for (const int fps : {90, 120})
            for (const auto *style : {"line", "points", "bars", "pie"}) {
                vector().set_render_fps(fps);
                vector().set_display_mode(style);
                require(vector().render_fps() == fps, "selected high frame rate lost on renderer switch");
            }
        vector().set_render_fps(60);
        vector().set_display_mode("bars");
        chart->set_x_range(true, 0, 1);
        chart->update(samples);
        require(wait([&] { return chart->projected_point_count() <= 2048 && chart->projected_point_count() > 0; }),
                "bars budget");
        const auto bars = chart->projected_points();
        require(std::any_of(bars.begin(), bars.end(), [](auto p) { return p.y() == .9f; }) &&
                    std::any_of(bars.begin(), bars.end(), [](auto p) { return p.y() == -.8f; }),
                "signed bars lost opposite extrema");
        chart->set_x_coordinates({});
        const auto plotLeft = chart->presentation_model()->state().value("plotLeft").toDouble();
        const auto resolvableBins =
            size_t(std::clamp((double(chart->width()) - plotLeft - 22) * chart->devicePixelRatioF(), 1., 2048.));
        std::vector<float> resolvedSpectrum(resolvableBins);
        for (size_t i = 0; i < resolvedSpectrum.size(); ++i)
            resolvedSpectrum[i] = float(i) / float(resolvedSpectrum.size());
        chart->update(resolvedSpectrum);
        require(wait([&] {
                    const auto points = chart->projected_points();
                    return points.size() == resolvableBins && points.back().y() == resolvedSpectrum.back();
                }),
                "bars discarded bins resolvable by physical viewport");
        const auto resolvedPoints = chart->projected_points();
        for (size_t i = 0; i < resolvedPoints.size(); ++i)
            require(resolvedPoints[i] == QPointF(double(i), resolvedSpectrum[i]),
                    "resolvable bar coordinate/value changed");
        vector().changePreference("bar_width_percent", 55.);
        require(wait([&] { return chart->presentation_model()->state().value("barWidth").toDouble() == 55; }),
                "bar width not applied");
        require(item(controls->rootObject(), "bar_width_percent") && !item(controls->rootObject(), "line_width"),
                "bars controls contain another mode");
    }

    void typedPainting() {
        // A bar occupies its selected fraction of a bin, and signed bars start
        // at the visible zero baseline. Verify actual pixels, not only state.
        VectorPlotItem paintedBars;
        paintedBars.setWidth(200);
        paintedBars.setHeight(100);
        paintedBars.setColors({{"highlight", QColor(Qt::red)}, {"mid", QColor(Qt::gray)}});
        QuickPlotModel typedModel;
        paintedBars.setSourceModel(&typedModel);
        typedModel.publishVector({{"kind", "vector"}, {"style", "bars"}, {"baseline", .5}, {"barWidth", 80}},
                                 QPolygonF{{.25, .1}, {.75, .9}});
        QImage barPixels(200, 100, QImage::Format_ARGB32_Premultiplied);
        barPixels.fill(Qt::transparent);
        {
            QPainter painter(&barPixels);
            paintedBars.paint(&painter);
        }
        require(barPixels.pixelColor(25, 25).alpha() > 0 && barPixels.pixelColor(175, 75).alpha() > 0 &&
                    barPixels.pixelColor(100, 25).alpha() == 0,
                "bars are missing or rendered as thin strokes");
        // Several GUI publications before a paint must consume the newest
        // immutable snapshot once, with sub-float coordinate precision intact.
        const auto retained = typedModel.vectorPoints();
        const double precise = .250000000123456;
        const auto consumed = paintedBars.consumedPresentationCount();
        for (int frame = 0; frame < 8; ++frame)
            typedModel.publishVector({{"kind", "vector"}, {"style", "points"}, {"pointSize", 12}},
                                     QPolygonF{{precise, .2 + frame * .05}, {.75, .75}});
        require(retained[0] == QPointF(.25, .1) && typedModel.vectorPoints()[0] == QPointF(precise, .55),
                "typed publication mutated retained snapshot or rounded coordinates");
        QImage pointPixels(200, 100, QImage::Format_ARGB32_Premultiplied);
        pointPixels.fill(Qt::transparent);
        {
            QPainter painter(&pointPixels);
            paintedBars.paint(&painter);
        }
        require(paintedBars.consumedPresentationCount() == consumed + 1 && pointPixels.pixelColor(50, 55).alpha() > 0 &&
                    pointPixels.pixelColor(150, 75).alpha() > 0 && pointPixels.pixelColor(100, 65).alpha() == 0,
                "typed point burst painted stale geometry or consumed intermediate frames");
        typedModel.publishVector({{"kind", "vector"}, {"style", "line"}, {"lineWidth", 4}},
                                 QPolygonF{{.25, .25}, {.75, .75}});
        QImage linePixels(200, 100, QImage::Format_ARGB32_Premultiplied);
        linePixels.fill(Qt::transparent);
        {
            QPainter painter(&linePixels);
            paintedBars.paint(&painter);
        }
        require(linePixels.pixelColor(100, 50).alpha() > 0 && linePixels.pixelColor(100, 65).alpha() == 0,
                "typed line geometry did not connect supplied points");
    }

    void vectorPreferences() {
        capture(chart, "bars");
        auto *mode = controls->rootObject()->findChild<QObject *>("numericDisplayMode");
        require(mode, "QML display selector missing");
        mode->setProperty("currentIndex", 1);
        require(QMetaObject::invokeMethod(mode, "activated", Q_ARG(int, 1)), "QML mode activation missing");
        require(vector().display_mode() == "points", "QML activation did not route to graph model");
        vector().changePreference("point_size", 8.);
        require(wait([&] { return chart->presentation_model()->state().value("pointSize").toDouble() == 8; }),
                "point size not applied");
        require(item(controls->rootObject(), "point_size") && !item(controls->rootObject(), "bar_width_percent"),
                "point controls contain another mode");
        vector().set_display_mode("line");
        require(chart->presentation_model()->state().value("lineWidth").toDouble() == 4,
                "mode switch lost line preference");
        vector().set_display_mode("points");
        capture(chart, "points");
        writesStorage = std::make_unique<QSignalSpy>(&vector(), &GraphPanel::presentation_changed);
        vector().restore_presentation({{"display_mode", "bars"},
                                       {"auto_range_x", false},
                                       {"minimum_x", 100},
                                       {"maximum_x", 500},
                                       {"reverse_x", true}});
        require(writes().isEmpty() && chart->reverse_x() && chart->x_range().first == 100 &&
                    chart->x_range().second == 500,
                "QML restore mutated preferences or lost axes");
        require(wait([&] { return mode->property("currentIndex").toInt() == 2; }),
                "QML selector binding lost on restore");
        const auto before = vector().preferences();
        vector().changePreference("line_width", 0.);
        vector().changePreference("point_size", 65.);
        vector().changePreference("bar_width_percent", 101.);
        vector().changePreference("pie_labels_visible", 1);
        require(vector().preferences() == before, "invalid series preferences accepted");
        vector().changePreference("reverse_x", QString("false"));
        vector().changePreference("minimum_x", QString("200"));
        require(vector().preferences() == before, "untyped preference accepted");
    }

    void pieProjection() {
        vector().set_display_mode("pie");
        auto *pie = vector().pie_view();
        ready(pie);
        pie->set_values({1, 3});
        require(pie->slice_count() == 2 && pie->slice_fraction(1) == .75, "QML pie proportions");
        auto *pieRenderer = item(pie->rootObject(), "numericPiePlot");
        require(pieRenderer && wait([&] { return pieRenderer->property("paintCount").toULongLong() > 0; }),
                "native pie did not paint");
        require(axisEditors->property("count").toInt() == 0, "pie exposes Cartesian axes");
        auto *pieLabels = item(controls->rootObject(), "pieSliceLabels");
        require(pieLabels && pieLabels->isVisible(), "pie labels control missing");
        vector().changePreference("pie_labels_visible", false);
        require(!pie->presentation_model()->state().value("showSliceLabels").toBool() && pie->slice_count() == 2,
                "label preference changed pie values");
        require(wait([&] { return pieRenderer->property("pieLabels").toList().isEmpty(); }),
                "pie labels remain after disabling");
        vector().changePreference("pie_labels_visible", true);
        require(wait([&] { return !pieRenderer->property("pieLabels").toList().isEmpty(); }),
                "pie labels did not return");
        capture(pie, "pie");
        capture(controls, "controls");
        require(!pie->set_values({-1}) && !pie->projection_available(), "QML pie invalid values accepted");
    }

    void coordinateOnlyDelivery() {
        // Coordinate-only changes and rejected oversize data must publish even
        // without another producer frame after dirty-triggered scheduling.
        vector().set_display_mode("line");
        chart->set_x_range(true, 0, 1);
        chart->update({1, 2});
        require(wait([&] { return chart->value_count() == 2 && chart->projected_point_count() == 2; }),
                "small vector did not publish");
        require(chart->set_x_coordinates({10, 20}), "coordinate-only input rejected");
        require(wait([&] {
                    const auto projected = chart->projected_points();
                    return projected.size() == 2 && projected[0].x() == 10 && projected[1].x() == 20;
                }),
                "coordinate-only dirty update never published");
        chart->update(std::vector<float>(1048577, 1));
        require(wait([&] { return chart->presentation_model()->vectorPoints().isEmpty(); }),
                "oversize rejection left stale vector geometry");
    }

    void matrixProjection() {
        matrixStorage = std::make_unique<GraphPanel>(
            "matrix", new LineChartView,
            QJsonObject{{"kind", "SpectrogramView"},
                        {"display_mode", "image2d"},
                        {"auto_range", false},
                        {"minimum", 0},
                        {"maximum", 6}},
            nullptr, nullptr,
            presentation::plot_capabilities(presentation::PlotDescriptor{{"image2d", "surface3d"}, {}, false}));
        noFocus(matrix());
        matrix().resize(800, 500);
        matrix().show();
        auto *image = matrix().image_view();
        ready(image);
        image->set_y_range(false, 0, 6, 10);
        image->set_matrix({{1, 2, 3}, {4, 5, 6}});
        const auto owner = image->projection_storage_identity();
        SpectrogramAxisMetadata axes;
        axes.age_axis = false;
        axes.column_coordinates = true;
        axes.columns = {70, 95, 180};
        axes.column_title_key = "Frequency, Hz (Z)";
        image->set_axis_metadata(axes);
        require(!matrix().physical_frequency(), "Unbound QML graph exposes physical frequency");
        matrix().set_axis_permissions({true, false});
        require(matrix().physical_frequency(), "QML source coordinate capability missing");
        matrixWritesStorage = std::make_unique<QSignalSpy>(&matrix(), &GraphPanel::presentation_changed);
        matrix().changePreference("frequency_axis_mode", "positions");
        require(image->axis_metadata().column(2) == 2, "QML positions choice changes no labels");
        image->set_axis_metadata({});
        require(!matrix().physical_frequency() && matrixWrites().size() == 1, "capability refresh wrote preferences");
        require(image->projection_storage_identity() == owner, "axis choice copied matrix snapshot");
        auto *raster = item(image->rootObject(), "numericRaster");
        require(raster && wait([&] { return raster->property("status").toInt() == 1; }),
                "QML numeric image did not load");
        auto pixels = image->grabFramebuffer();
        require(!pixels.isNull(), "QML image framebuffer missing");
        const auto displayed = [&](double x, double y) {
            const auto point = raster->mapToScene({raster->width() * x, raster->height() * y});
            return pixels.pixelColor(int(point.x() * pixels.width() / image->width()),
                                     int(point.y() * pixels.height() / image->height()));
        };
        const auto matches = [](QColor a, QColor b) {
            return std::abs(a.red() - b.red()) <= 2 && std::abs(a.green() - b.green()) <= 2 &&
                   std::abs(a.blue() - b.blue()) <= 2;
        };
        require(matches(displayed(1. / 6, 1. / 4), image->numeric_raster().pixelColor(0, 0)) &&
                    matches(displayed(5. / 6, 3. / 4), image->numeric_raster().pixelColor(2, 1)),
                "QML displayed raster differs from numeric cells");
        capture(image, "image2d");
    }

    void surfaceRestoration() {
        // Per-view surface state survives lazy renderer creation without writing a document delta.
        const QJsonObject savedCamera{
            {"version", 2}, {"position", QJsonArray{2, 3, 4}}, {"quaternion", QJsonArray{1, 0, 0, 0}}};
        surfaceStorage = std::make_unique<GraphPanel>(
            "surface", new LineChartView, QJsonObject{{"kind", "SpectrogramView"}, {"display_mode", "image2d"}},
            nullptr, nullptr,
            presentation::plot_capabilities(presentation::PlotDescriptor{{"image2d", "surface3d"}, {}, false}));
        surfaceWritesStorage = std::make_unique<QSignalSpy>(&surface(), &GraphPanel::presentation_changed);
        surface().restore_presentation({{"display_mode", "image2d"},
                                        {"camera", savedCamera},
                                        {"renderer", QJsonObject{{"pause", true}, {"selection", 1}}},
                                        {"surface_side_ratio", .35}});
        require(surfaceWrites().isEmpty() && !surface().owned_surface(),
                "surface restore eagerly creates renderer or writes state");
        surface().set_display_mode("surface3d");
        surfaceView = surface().owned_surface();
        require(surfaceView && surfaceView->paused(), "lazy surface lost pause state");
        require(surfaceView->controls_embedded() && surface().surface_model() == surfaceView,
                "surface controls not embedded in the shared graph sidebar");
        require(surfaceView->camera_state().value("position") == savedCamera.value("position"),
                "lazy surface lost camera");
        require(std::abs(surfaceView->quick_state().value("panelRatio").toDouble() - .35) < 1e-5,
                "lazy surface lost side ratio");
        surfaceWrites().clear();
        surfaceView->quick_action("pause", false);
        const auto persistedSurface = surface().preferences().value("renderer").toMap();
        require(surfaceWrites().size() == 1 && persistedSurface.contains("pause") &&
                    persistedSurface.value("pause").metaType().id() == QMetaType::Bool &&
                    !persistedSurface.value("pause").toBool() && persistedSurface.value("selection").toInt() == 1,
                "surface action not persisted once");
        surfaceView->set_y_range(0, 10);
        surfaceView->set_history_dimensions(8, 2);
        surfaceHost = surfaceView->findChild<QQuickWidget *>("spectrogramQuickView");
        ready(surfaceHost);
        surfaceOverlay = qobject_cast<SurfaceOverlayItem *>(item(surfaceHost->rootObject(), "spectrogramOverlay"));
        require(surfaceOverlay && surfaceOverlay->sourceModel() == surfaceView,
                "3D overlay must use direct native model delivery");
    }

    void overlayRendering() {
        overlayHostStorage = std::make_unique<QQuickWidget>();
        noFocus(overlayHost());
        overlayHost().setClearColor(Qt::transparent);
        overlayHost().resize(180, 160);
        overlayHost().setResizeMode(QQuickWidget::SizeRootObjectToView);
        auto *overlayComponent = new QQmlComponent(overlayHost().engine(), &overlayHost());
        overlayComponent->setData("import QtQuick\nimport Barelief.Surface 1.0\nSurfaceOverlay {width:180;height:160}",
                                  QUrl());
        auto *nativeOverlay = qobject_cast<SurfaceOverlayItem *>(overlayComponent->create());
        require(nativeOverlay, "native overlay fixture creation failed");
        overlayHost().setContent(QUrl(), overlayComponent, nativeOverlay);
        overlayHost().show();
        ready(&overlayHost());

        overlayListStorage = std::make_unique<SurfaceOverlayCommands>(QApplication::font());
        overlayList().setPen(QPen(Qt::red, 20));
        overlayList().drawLine({30, 40}, {80, 40});
        nativeOverlay->setCommands(overlayList().commands);
        auto overlayImage = overlayPixels();
        if (argc == 2)
            overlayImage.save(artifacts.filePath("overlay-stroke.png"));
        std::printf("overlay fixture: %dx%d item %.0fx%.0f engine %d pixel %s alpha %d\n", overlayImage.width(),
                    overlayImage.height(), nativeOverlay->width(), nativeOverlay->height(),
                    nativeOverlay->paintEngineType(),
                    overlayColor(overlayImage, 50, 32).name().toLocal8Bit().constData(),
                    overlayColor(overlayImage, 50, 32).alpha());
        require(overlayColor(overlayImage, 50, 32).red() > 240 && overlayColor(overlayImage, 50, 32).alpha() > 240,
                "native thick overlay stroke missing");
        SurfaceOverlayCommands movedOverlay(QApplication::font());
        movedOverlay.setPen(QPen(Qt::blue, 20));
        movedOverlay.drawLine({100, 100}, {150, 100});
        nativeOverlay->setCommands(movedOverlay.commands);
        overlayImage = overlayPixels();
        require(overlayColor(overlayImage, 50, 32).alpha() == 0 && overlayColor(overlayImage, 120, 92).blue() > 240,
                "native overlay partial update left old stroke or missed new edge");
        overlayCommandWritesStorage = std::make_unique<QSignalSpy>(nativeOverlay, &SurfaceOverlayItem::commandsChanged);
        nativeOverlay->setCommands(movedOverlay.commands);
        require(overlayCommandWrites().isEmpty(), "unchanged overlay republished");
        nativeOverlay->setCommands({});
        overlayImage = overlayPixels();
        require(overlayColor(overlayImage, 120, 92).alpha() == 0, "removed overlay left pixels in damaged region");
        overlayHost().hide();
    }

    void surfaceCamera() {
        cameraPanelNotificationsStorage = std::make_unique<QSignalSpy>(&surface(), &GraphPanel::state_changed);
        surfaceWrites().clear();
        const auto oldCamera = surfaceView->camera_state();
        surfaceView->quick_pointer(0, 100, 100, Qt::RightButton, Qt::RightButton, Qt::NoModifier);
        surfaceView->quick_pointer(1, 120, 110, Qt::NoButton, Qt::RightButton, Qt::NoModifier);
        require(surfaceView->camera_state() != oldCamera && surfaceWrites().size() == 1 &&
                    cameraPanelNotifications().isEmpty(),
                "camera input rebuilt sidebar controls or failed to publish persistence");
        require(surface().preferences().value("camera").toMap() == surfaceView->camera_state().toVariantMap(),
                "camera pose not retained in graph preferences");
        surfaceWrites().clear();
        surfaceView->quick_pointer(1, 120, 110, Qt::NoButton, Qt::RightButton, Qt::NoModifier);
        surfaceView->quick_wheel(0, 0, false);
        require(surfaceWrites().isEmpty(), "zero camera input publishes redundant pose");
        surfaceView->quick_pointer(2, 120, 110, Qt::RightButton, Qt::NoButton, Qt::NoModifier);
    }

    void surfaceAxes() {
        SpectrogramAxisMetadata cameraAxes;
        cameraAxes.column_coordinates = true;
        cameraAxes.column_title_key = "Frequency, Hz (Z)";
        cameraAxes.columns = {10, 20};
        cameraAxes.time_coordinates = true;
        cameraAxes.time_title_key = "Time delay, seconds (X)";
        cameraAxes.ages = {0, 1, 2, 3, 4, 5, 6, 7};
        surfaceView->set_axis_metadata(cameraAxes);
        require(!surface().physical_frequency() && !surface().physical_time(), "Unbound surface exposes physical axes");
        surface().set_axis_permissions({true, true});
        require(surface().physical_frequency() && surface().physical_time() && !cameraPanelNotifications().isEmpty(),
                "physical axis capabilities not published");
        cameraPanelNotifications().clear();
        QSignalSpy settingsUpdates(surfaceView,&SpectrogramGraph::quick_state_changed);
        const auto previousSettings=surfaceView->quick_state();
        cameraAxes.ages = {0, 2, 4, 6, 8, 10, 12, 14};
        surfaceView->set_axis_metadata(cameraAxes);
        require(cameraPanelNotifications().isEmpty(), "streaming axis timestamps rebuilt sidebar controls");
        require(surfaceView->projected_axis_metadata().ages==cameraAxes.ages,
                "streaming coordinates did not reach the renderer");
        require(surfaceView->quick_state()==previousSettings && settingsUpdates.isEmpty(),
                "streaming coordinates republished unchanged surface settings");
        cameraAxes.column_coordinates = false;
        surfaceView->set_axis_metadata(cameraAxes);
        require(!surface().physical_frequency() && cameraPanelNotifications().size() == 1,
                "changed axis capability left stale selector");
    }

    void surfaceFeedback() {
        snapshotNotificationsStorage =
            std::make_unique<QSignalSpy>(surfaceView, &SpectrogramGraph::quick_state_changed);
        surfaceView->update_frame_history({{1, 2}, {3, 4}});
        require(!snapshotNotifications().isEmpty() && wait([&] {
            return surfaceHost->rootObject()->property("state").toMap().value("historyFrames").toULongLong() == 2;
        }),
                "surface snapshot did not refresh QML history count");
        surfaceWrites().clear();
        surface().restore_presentation({{"display_mode", "surface3d"}, {"renderer", QJsonObject{{"pause", true}}}});
        require(surfaceWrites().isEmpty() && surfaceView->paused(), "surface restore emitted state writes");
        overlayNotificationsStorage = std::make_unique<QSignalSpy>(surfaceView, &SpectrogramGraph::overlay_changed);
        QVariantMap feedback{{"overlayCommands", overlayList().commands}, {"renderedFrames", qulonglong(42)}};
        surfaceView->accept_render_feedback(feedback);
        require(surfaceOverlay->commands() == overlayList().commands,
                "native overlay did not consume model feedback directly");
        feedback["renderedFrames"] = qulonglong(43);
        surfaceView->accept_render_feedback(feedback);
        require(overlayNotifications().size() == 1 &&
                    surfaceView->axis_layout_telemetry().value("paintCount").toULongLong() == 43,
                "identical surface overlay repainted or feedback acknowledgement was lost");
    }

    void metricsValues() {
        metricsStorage = std::make_unique<GraphPanel>(
            "metrics", new LineChartView, QJsonObject{{"kind", "ScalarView"}}, nullptr, nullptr,
            presentation::plot_capabilities(presentation::PlotDescriptor{{"metrics", "signedbar"}, {}, false}));
        noFocus(metrics());
        metrics().resize(800, 400);
        metrics().show();
        values = metrics().metrics_view();
        ready(values);
        values->set_inputs({{0, "<b>Literal name</b>"}, {9, "Maximum"}});
        values->set_value(0, "9007199254740993");
        values->set_value(9, "18446744073709551615");
        capture(values, "metrics");
        require(wait([&] {
                    auto *text = item(values->rootObject(), "metricValue_9");
                    return text && text->property("text").toString() == "18446744073709551615";
                }),
                "QML rounded exact UInt64 or delegate missing");
        const QPointer<QQuickItem> retainedMetric = item(values->rootObject(), "metricValue_9");
        const QPointer<QQuickItem> changedMetric = item(values->rootObject(), "metricValue_0");
        metricsResetStorage = std::make_unique<QSignalSpy>(values->metrics_model(), &QAbstractItemModel::modelReset);
        metricsPublicationsStorage =
            std::make_unique<QSignalSpy>(values->presentation_model(), &QuickPlotModel::changed);
        for (quint64 i = 0; i < 32; ++i)
            values->set_value(0, QString::number(9007199254740993ULL + i));
        values->set_inputs({{0, "<b>Literal name</b>"}, {9, "Renamed"}});
        QCoreApplication::processEvents();
        require(metricsReset().isEmpty() && metricsPublications().isEmpty() && retainedMetric &&
                    retainedMetric == item(values->rootObject(), "metricValue_9") &&
                    retainedMetric->property("text").toString() == "18446744073709551615" && changedMetric &&
                    changedMetric == item(values->rootObject(), "metricValue_0") &&
                    changedMetric->property("text").toString() == "9007199254741024" &&
                    retainedMetric->parentItem()->property("metricName").toString() == "Renamed",
                "metric updates rebuilt delegates or changed unrelated exact values");
    }

    int finishMetrics() {
        values->set_value(0, "9007199254740993");
        for (const auto *language : {"ru", "en"})
            for (const auto *theme : {"dark", "light"}) {
                ui::Language::instance().set(language);
                ui::Theme::instance().set(theme);
                QCoreApplication::processEvents();
                require(values->exact_value(0) == "9007199254740993" && values->input_name(0) == "<b>Literal name</b>",
                        "theme/language changed user data");
                const auto colors = values->rootObject()->property("colors").toMap();
                require(colors.value("base").value<QColor>() == ui::Theme::instance().palette().color(QPalette::Base),
                        "QML palette does not follow Theme");
            }
        values->set_unavailable(9);
        require(!values->value_available(9), "QML unavailable metric stale");
        for (const int fps : {90, 120}) {
            matrix().set_render_fps(fps);
            for (const auto *mode : {"image2d", "surface3d"}) {
                matrix().set_display_mode(mode);
                require(matrix().render_fps() == fps, "matrix/surface switch lost high frame rate");
            }
            metrics().set_render_fps(fps);
            require(metrics().render_fps() == fps, "metrics lost high frame rate");
        }
        vector().hide();
        matrix().hide();
        metrics().hide();
        std::puts("QML graphs PASS: scenes, controls, bounded peaks, exact scalars, axes, theme/language, restore");
        return 0;
    }

  public:
    QmlGraphScenario(int count, char **argv)
        : argc(count), artifacts(count == 2 ? QString::fromLocal8Bit(argv[1]) : QString{}) {}
    int run() {
        setupVector();
        diagnosticNotificationsCase();
        informationNotificationsCase();
        vectorProjection();
        unchangedVectorIngress();
        extremeVectorGeometry();
        vectorBurst();
        pendingVectorCadence();
        vectorModesAndBins();
        typedPainting();
        vectorPreferences();
        pieProjection();
        coordinateOnlyDelivery();
        matrixProjection();
        surfaceRestoration();
        overlayRendering();
        surfaceCamera();
        surfaceAxes();
        surfaceFeedback();
        metricsValues();
        return finishMetrics();
    }
};
} // namespace
int main(int argc, char **argv) {
    ui::quick::configure();
    if (QSurfaceFormat::defaultFormat().swapInterval() != 0) {
        std::fprintf(stderr, "graph composition must use application pacing\n");
        return 1;
    }
    QApplication app(argc, argv);
    try {
        return QmlGraphScenario(argc, argv).run();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "QML graphs FAIL: %s\n", error.what());
        return 1;
    }
}
