#include <core_app/graph_panel.h>
#include <visualization/charts/line.h>
#include <visualization/numeric_image_view.h>
#include <visualization/numeric_metrics_view.h>
#include <visualization/numeric_pie_view.h>
#include <visualization/quick/quick_ui.h>
#include <visualization/spectrograms/spectrogram_graph.h>
#include <QApplication>
#include <QQuickItem>
#include <QElapsedTimer>
#include <QThread>
#include <QSignalSpy>
#include <QJsonArray>
#include <QDir>
#include <cstdio>
#include <stdexcept>

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool wait(const std::function<bool()>& ready){QElapsedTimer time;time.start();while(!ready() && time.elapsed()<3000){QCoreApplication::processEvents();QThread::msleep(2);}return ready();}
void ready(QQuickWidget* view){require(view && wait([&]{return view->status()==QQuickWidget::Ready;}),"QML scene failed to load");require(view->property("qmlErrors").toStringList().isEmpty(),"QML error telemetry nonempty");}
QQuickItem* item(QQuickItem* root,const QString& name){if(!root)return nullptr;if(root->objectName()==name)return root;for(auto* child:root->childItems())if(auto* found=item(child,name))return found;return nullptr;}
}
int main(int argc,char** argv){
    ui::quick::configure();
    QApplication app(argc,argv);
    try {
        QDir artifacts(argc==2?QString::fromLocal8Bit(argv[1]):QString{});
        const auto capture=[&](QQuickWidget* view,const QString& name){if(argc!=2)return;require(artifacts.mkpath("."),"artifact directory");QElapsedTimer timer;timer.start();while(timer.elapsed()<100){QCoreApplication::processEvents();QThread::msleep(2);}require(view->grabFramebuffer().save(artifacts.filePath(name+".png")),"save own QML framebuffer");};
        const auto noFocus=[](QWidget& view){view.setAttribute(Qt::WA_ShowWithoutActivating);view.setWindowFlag(Qt::WindowDoesNotAcceptFocus);};
        GraphPanel vector("vector",new LineChartView,{{"kind","FFTView"}},nullptr);
        noFocus(vector);
        vector.resize(900,500);vector.show();
        auto* controls=vector.findChild<QQuickWidget*>("numericGraphSettings");ready(controls);
        auto* chart=vector.active_chart();ready(chart);
        chart->set_auto_resizing(false);chart->set_range(-1,1);
        std::vector<float> samples(1000000,0);samples[345678]=.9f;samples[345679]=-.8f;
        chart->update(samples);
        require(wait([&]{return chart->projected_point_count()>0;}),"vector projection missing");
        require(chart->value_count()==1000000 && chart->projected_point_count()<=4098,"QML vector budget not bounded");
        const auto points=chart->projected_points();
        require(std::any_of(points.begin(),points.end(),[](auto p){return p.y()==.9f;}) && std::any_of(points.begin(),points.end(),[](auto p){return p.y()==-.8f;}),"display decimation lost peaks");
        capture(chart,"line");
        vector.set_display_mode("bars");chart->set_x_range(true,0,1);chart->update(samples);
        require(wait([&]{return chart->projected_point_count()<=2048 && chart->projected_point_count()>0;}),"bars budget");
        const auto bars=chart->projected_points();require(std::any_of(bars.begin(),bars.end(),[](auto p){return p.y()==.9f;}) && std::any_of(bars.begin(),bars.end(),[](auto p){return p.y()==-.8f;}),"signed bars lost opposite extrema");
        capture(chart,"bars");
        auto* mode=controls->rootObject()->findChild<QObject*>("numericDisplayMode");require(mode,"QML display selector missing");
        mode->setProperty("currentIndex",1);
        require(QMetaObject::invokeMethod(mode,"activated",Q_ARG(int,1)),"QML mode activation missing");
        require(vector.display_mode()=="points","QML activation did not route to graph model");
        capture(chart,"points");
        QSignalSpy writes(&vector,&GraphPanel::presentation_changed);
        vector.restore_presentation({{"display_mode","bars"},{"auto_range_x",false},{"minimum_x",100},{"maximum_x",500},{"reverse_x",true}});
        require(writes.isEmpty() && chart->reverse_x() && chart->x_range().first==100 && chart->x_range().second==500,"QML restore mutated preferences or lost axes");
        require(wait([&]{return mode->property("currentIndex").toInt()==2;}),"QML selector binding lost on restore");
        const auto before=vector.preferences();vector.changePreference("reverse_x",QString("false"));vector.changePreference("minimum_x",QString("200"));
        require(vector.preferences()==before,"untyped preference accepted");
        vector.set_display_mode("pie");auto* pie=vector.pie_view();ready(pie);pie->set_values({1,3});
        require(pie->slice_count()==2 && pie->slice_fraction(1)==.75,"QML pie proportions");
        capture(pie,"pie");capture(controls,"controls");
        require(!pie->set_values({-1}) && !pie->projection_available(),"QML pie invalid values accepted");

        GraphPanel matrix("matrix",new LineChartView,{{"kind","SpectrogramView"},{"display_mode","image2d"},{"auto_range",false},{"minimum",0},{"maximum",6}},nullptr);
        noFocus(matrix);
        matrix.resize(800,500);matrix.show();auto* image=matrix.image_view();ready(image);
        image->set_y_range(false,0,6,10);image->set_matrix({{1,2,3},{4,5,6}});
        const auto owner=image->projection_storage_identity();
        SpectrogramAxisMetadata axes;axes.age_axis=false;axes.column_coordinates=true;axes.columns={70,95,180};axes.column_title_key="Frequency, Hz (Z)";
        image->set_axis_metadata(axes);require(matrix.physical_frequency(),"QML source coordinate capability missing");
        QSignalSpy matrixWrites(&matrix,&GraphPanel::presentation_changed);
        matrix.changePreference("frequency_axis_mode","positions");require(image->axis_metadata().column(2)==2,"QML positions choice changes no labels");
        image->set_axis_metadata({});require(!matrix.physical_frequency() && matrixWrites.size()==1,"capability refresh wrote preferences");
        require(image->projection_storage_identity()==owner,"axis choice copied matrix snapshot");
        auto* raster=item(image->rootObject(),"numericRaster");require(raster && wait([&]{return raster->property("status").toInt()==1;}),"QML numeric image did not load");
        auto pixels=image->grabFramebuffer();require(!pixels.isNull(),"QML image framebuffer missing");
        const auto displayed=[&](double x,double y){const auto point=raster->mapToScene({raster->width()*x,raster->height()*y});return pixels.pixelColor(int(point.x()*pixels.width()/image->width()),int(point.y()*pixels.height()/image->height()));};
        const auto matches=[](QColor a,QColor b){return std::abs(a.red()-b.red())<=2 && std::abs(a.green()-b.green())<=2 && std::abs(a.blue()-b.blue())<=2;};
        require(matches(displayed(1./6,1./4),image->numeric_raster().pixelColor(0,0)) && matches(displayed(5./6,3./4),image->numeric_raster().pixelColor(2,1)),"QML displayed raster differs from numeric cells");
        capture(image,"image2d");

        // Per-view surface state survives lazy renderer creation without writing a document delta.
        const QJsonObject savedCamera{{"version",2},{"position",QJsonArray{2,3,4}},{"quaternion",QJsonArray{1,0,0,0}}};
        GraphPanel surface("surface",new LineChartView,{{"kind","SpectrogramView"},{"display_mode","image2d"}},nullptr);
        QSignalSpy surfaceWrites(&surface,&GraphPanel::presentation_changed);
        surface.restore_presentation({{"display_mode","image2d"},{"camera",savedCamera},{"renderer",QJsonObject{{"pause",true},{"selection",1}}},{"surface_side_ratio",.35}});
        require(surfaceWrites.isEmpty() && !surface.owned_surface(),"surface restore eagerly creates renderer or writes state");
        surface.set_display_mode("surface3d");
        auto* surfaceView=surface.owned_surface();require(surfaceView && surfaceView->paused(),"lazy surface lost pause state");
        require(surfaceView->camera_state().value("position")==savedCamera.value("position"),"lazy surface lost camera");
        require(std::abs(surfaceView->quick_state().value("panelRatio").toDouble()-.35)<1e-5,"lazy surface lost side ratio");
        surfaceWrites.clear();surfaceView->quick_action("pause",false);
        const auto persistedSurface=surface.preferences().value("renderer").toMap();
        require(surfaceWrites.size()==1 && persistedSurface.contains("pause") && persistedSurface.value("pause").metaType().id()==QMetaType::Bool && !persistedSurface.value("pause").toBool() && persistedSurface.value("selection").toInt()==1,"surface action not persisted once");
        surfaceView->set_y_range(0,10);surfaceView->set_history_dimensions(8,2);
        auto* surfaceHost=surfaceView->findChild<QQuickWidget*>("spectrogramQuickView");ready(surfaceHost);
        QSignalSpy snapshotNotifications(surfaceView,&SpectrogramGraph::quick_state_changed);
        surfaceView->update_frame_history({{1,2},{3,4}});
        require(!snapshotNotifications.isEmpty() && wait([&]{return surfaceHost->rootObject()->property("state").toMap().value("historyFrames").toULongLong()==2;}),"surface snapshot did not refresh QML history count");
        surfaceWrites.clear();surface.restore_presentation({{"display_mode","surface3d"},{"renderer",QJsonObject{{"pause",true}}}});
        require(surfaceWrites.isEmpty() && surfaceView->paused(),"surface restore emitted state writes");

        GraphPanel metrics("metrics",new LineChartView,{{"kind","ScalarView"}},nullptr);
        noFocus(metrics);
        metrics.resize(800,400);metrics.show();auto* values=metrics.metrics_view();ready(values);
        values->set_inputs({{0,"<b>Literal name</b>"},{9,"Maximum"}});values->set_value(0,"9007199254740993");values->set_value(9,"18446744073709551615");
        capture(values,"metrics");
        require(wait([&]{auto* text=item(values->rootObject(),"metricValue_9");return text && text->property("text").toString()=="18446744073709551615";}),"QML rounded exact UInt64 or delegate missing");
        for(const auto* language:{"ru","en"})for(const auto* theme:{"dark","light"}){
            ui::Language::instance().set(language);ui::Theme::instance().set(theme);QCoreApplication::processEvents();
            require(values->exact_value(0)=="9007199254740993" && values->input_name(0)=="<b>Literal name</b>","theme/language changed user data");
            const auto colors=values->rootObject()->property("colors").toMap();
            require(colors.value("base").value<QColor>()==ui::Theme::instance().palette().color(QPalette::Base),"QML palette does not follow Theme");
        }
        values->set_unavailable(9);require(!values->value_available(9),"QML unavailable metric stale");
        vector.hide();matrix.hide();metrics.hide();
        std::puts("QML graphs PASS: scenes, controls, bounded peaks, exact scalars, axes, theme/language, restore");return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"QML graphs FAIL: %s\n",error.what());return 1;}
}
