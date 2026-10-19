#include <core_app/graph_panel.h>
#include <barelief/charts/line.h>
#include <barelief/numeric_image_view.h>
#include <barelief/numeric_pie_view.h>
#include <barelief/numeric_metrics_view.h>
#include <barelief/spectrograms/spectrogram_graph.h>
#include <barelief/ui_text.h>
#include <QApplication>
#include <QAbstractItemView>
#include <QComboBox>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QLabel>
#include <QPainter>
#include <QScreen>
#include <QScrollArea>
#include <QThread>
#include <QTimer>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#ifdef NUMERIC_QML
#include <barelief/quick/quick_ui.h>
#include <QQuickWidget>
#include <QQuickItem>
#include <QtTest/QTest>
#else
#include <QOpenGLWidget>
#endif

namespace {
void check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void settle(int ms=250){QElapsedTimer timer;timer.start();while(timer.elapsed()<ms){QCoreApplication::processEvents();QThread::msleep(3);}}
QJsonArray rect(const QRectF& r){return {r.x(),r.y(),r.width(),r.height()};}
QImage capture(QWidget* widget){
    auto image=widget->grab().toImage();
    QPainter painter(&image);
#ifdef NUMERIC_QML
    for(auto* child:widget->findChildren<QQuickWidget*>())if(child->isVisible()){
        const auto pixels=child->grabFramebuffer();check(!pixels.isNull(),"Quick framebuffer missing");
        painter.drawImage(QRect(child->mapTo(widget,QPoint()),child->size()),pixels);
    }
#else
    for(auto* child:widget->findChildren<QOpenGLWidget*>())if(child->isVisible()){
        const auto pixels=child->grabFramebuffer();check(!pixels.isNull(),"OpenGL framebuffer missing");
        painter.drawImage(QRect(child->mapTo(widget,QPoint()),child->size()),pixels);
    }
#endif
    return image;
}
#ifdef NUMERIC_QML
void collectQuick(QQuickItem* item,QJsonArray& controls,QQuickWidget* host,GraphPanel& panel){
    if(!item || !item->isVisible())return;
    QRectF visible=QRectF(item->mapToScene(QPointF()),QSizeF(item->width(),item->height())).intersected(QRectF(host->rect()));
    for(auto* ancestor=item->parentItem();ancestor;ancestor=ancestor->parentItem())if(ancestor->clip())visible=visible.intersected(QRectF(ancestor->mapToScene(QPointF()),QSizeF(ancestor->width(),ancestor->height())));
    const auto text=item->property("displayText").isValid()?item->property("displayText").toString():item->property("text").toString();
    if(!visible.isEmpty() && !text.isEmpty()){
        const auto font=item->property("font").value<QFont>();
        controls.append(QJsonObject{{"class",item->metaObject()->className()},{"name",item->objectName()},{"text",text},
            {"bounds",rect(QRectF(QPointF(host->mapTo(&panel,QPoint()))+item->mapToScene(QPointF()),QSizeF(item->width(),item->height())))},
            {"visibleBounds",rect(visible.translated(host->mapTo(&panel,QPoint())))},
            {"fontHeight",QFontMetricsF(font).height()},{"textWidth",QFontMetricsF(font).horizontalAdvance(text)},
            {"implicitWidth",item->implicitWidth()},{"implicitHeight",item->implicitHeight()}});
    }
    for(auto* child:item->childItems())collectQuick(child,controls,host,panel);
}
#endif
QJsonObject inspect(GraphPanel& panel,const QString& mode){
    auto* settings=panel.findChild<QWidget*>("numericGraphSettings");
    check(settings,"graph settings absent");
    QWidget* renderer=mode=="surface3d"?static_cast<QWidget*>(panel.active_spectrogram()):mode=="image2d"?static_cast<QWidget*>(panel.image_view()):mode=="pie"?static_cast<QWidget*>(panel.pie_view()):mode=="metrics"?static_cast<QWidget*>(panel.metrics_view()):static_cast<QWidget*>(panel.active_chart());
    check(renderer && renderer->isVisible(),"active renderer missing");
    QRectF plot(renderer->mapTo(&panel,QPoint()),renderer->size());
    QJsonArray controls,errors;
#ifdef NUMERIC_QML
    for(auto* host:panel.findChildren<QQuickWidget*>())if(host->isVisible()){
        check(host->status()==QQuickWidget::Ready,"QML scene not Ready");
        for(const auto& error:host->property("qmlErrors").toStringList())errors.append(error);
        collectQuick(host->rootObject(),controls,host,panel);
    }
    if(mode=="surface3d"){
        auto* host=renderer->findChild<QQuickWidget*>("spectrogramQuickView");
        auto* item=host->rootObject()->findChild<QQuickItem*>("spectrogramContainer");
        check(item,"surface Quick item absent");
        plot=QRectF(QPointF(host->mapTo(&panel,QPoint()))+item->mapToScene(QPointF()),QSizeF(item->width(),item->height()));
    }
#else
    for(auto* widget:panel.findChildren<QWidget*>())if(widget->isVisible()){
        QString text;
        if(auto* combo=qobject_cast<QComboBox*>(widget))text=combo->currentText();
        else if(auto* label=qobject_cast<QLabel*>(widget))text=label->text();
        else text=widget->property("text").toString();
        if(!text.isEmpty())controls.append(QJsonObject{{"class",widget->metaObject()->className()},{"name",widget->objectName()},
            {"text",text},{"bounds",rect(QRectF(widget->mapTo(&panel,QPoint()),widget->size()))},
            {"visibleBounds",rect(QRectF(widget->mapTo(&panel,widget->visibleRegion().boundingRect().topLeft()),widget->visibleRegion().boundingRect().size()))},
            {"fontHeight",QFontMetricsF(widget->font()).height()},{"textWidth",QFontMetricsF(widget->font()).horizontalAdvance(text)}});
    }
    if(mode=="surface3d"){
        auto* gl=renderer->findChild<QOpenGLWidget*>("spectrogramContainer");check(gl,"baseline surface GL widget absent");
        plot=QRectF(gl->mapTo(&panel,QPoint()),gl->size());
    }
#endif
    const auto visible=plot.intersected(QRectF(panel.rect()));
    auto result=QJsonObject{{"mode",mode},{"panel",rect(panel.rect())},{"dpr",panel.devicePixelRatioF()},{"plot",rect(plot)},
        {"visiblePlot",rect(visible)},{"visiblePlotFraction",visible.width()*visible.height()/double(panel.width()*panel.height())},
        {"allocationScope",mode=="surface3d"?"actual GL viewport":"renderer allocation including labels and internal margins"},
        {"logicalDpi",panel.logicalDpiX()},{"settingsVisible",settings->isVisible()},
        {"settings",rect(QRectF(settings->mapTo(&panel,QPoint()),settings->size()))},{"controls",controls},{"qmlErrors",errors}};
    if(mode=="surface3d")result["surfaceTelemetry"]=QJsonObject::fromVariantMap(panel.active_spectrogram()->axis_layout_telemetry());
    return result;
}
}
int main(int argc,char** argv){
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
#ifdef NUMERIC_QML
    ui::quick::configure();
#endif
    QApplication app(argc,argv);
    QTimer safety;QObject::connect(&safety,&QTimer::timeout,[]{std::_Exit(3);});safety.start(55000);
    try{
        check(argc==2,"provide artifact directory");QDir artifacts(QString::fromLocal8Bit(argv[1]));check(artifacts.mkpath("."),"artifact directory");
        ui::Language::instance().set("ru");
        QJsonArray cases;
        for(const auto* theme:{"dark","light"})for(const auto size:{QSize(1280,800),QSize(650,600)})for(const auto* rawMode:{"line","points","bars","pie","image2d","metrics","surface3d"}){
            const QString mode=QString::fromLatin1(rawMode),kind=mode=="metrics"?"ScalarView":(mode=="surface3d"||mode=="image2d")?"SpectrogramView":"FFTView";
            ui::Theme::instance().set(theme);
            GraphPanel panel("visualCompare",new LineChartView,{{"kind",kind},{"display_mode",mode}},nullptr);
            panel.setAttribute(Qt::WA_ShowWithoutActivating);panel.setWindowFlag(Qt::WindowDoesNotAcceptFocus,true);panel.resize(size);
            std::vector<float> tones(256);std::vector<std::vector<float>> history(80,std::vector<float>(50));
            for(size_t i=0;i<tones.size();++i)for(int tone=0;tone<8;++tone)tones[i]+=float(.08*std::sin((tone+1)*i*.05));
            for(size_t t=0;t<history.size();++t)for(size_t b=0;b<50;++b)history[t][b]=float(.2+.8*std::abs(std::sin(t*.09+b*.13)));
            if(mode=="surface3d"){
                auto* view=panel.active_spectrogram();check(view,"surface absent");view->set_y_range(0,1.2);view->set_history_dimensions(80,50);view->update_frame_history(history);
                SpectrogramAxisMetadata axes;axes.age_axis=true;axes.time_coordinates=true;axes.column_coordinates=true;
                for(int i=0;i<50;++i)axes.columns.push_back(50.+i*160.);for(int i=0;i<80;++i)axes.ages.push_back(i*.02);view->set_axis_metadata(axes);
            }else if(mode=="image2d")panel.image_view()->set_matrix(history);
            else if(mode=="pie")panel.pie_view()->set_values({1,2,3,4,5,6,7,8});
            else if(mode=="metrics"){panel.metrics_view()->set_inputs({{0,"Уровень сигнала"},{1,"Точное целое значение"}});panel.metrics_view()->set_value(0,"0.314159");panel.metrics_view()->set_value(1,"18446744073709551615");}
            else {panel.active_chart()->set_auto_resizing(false);panel.active_chart()->set_range(-1,1);panel.active_chart()->update(tones);}
            panel.show();settle();
            if(mode=="surface3d"){
                QElapsedTimer deadline;deadline.start();
                while(!panel.active_spectrogram()->axis_layout_telemetry().value("paintCount").toULongLong() && deadline.elapsed()<5000)settle(20);
                check(panel.active_spectrogram()->axis_layout_telemetry().value("paintCount").toULongLong()>0,"native surface never rendered");
            }
            const auto name=QString("%1-%2-%3").arg(theme).arg(size.width()).arg(mode);
            auto report=inspect(panel,mode);report["case"]=name;report["theme"]=theme;
            check(capture(&panel).save(artifacts.filePath(name+".png")),"save panel framebuffer");
            // Open the actual display-mode popup through its public UI object.
#ifdef NUMERIC_QML
            auto* settings=panel.findChild<QQuickWidget*>("numericGraphSettings");
            auto* combo=settings->rootObject()->findChild<QObject*>("numericDisplayMode");check(combo,"QML mode combo absent");
            auto* popup=combo->property("popup").value<QObject*>();check(popup,"QML mode popup absent");
            if(combo->property("enabled").toBool()){
                auto* comboItem=qobject_cast<QQuickItem*>(combo);check(comboItem,"QML combo is not a visual item");
                QTest::mouseClick(settings,Qt::LeftButton,Qt::NoModifier,comboItem->mapToScene(QPointF(comboItem->width()/2,comboItem->height()/2)).toPoint());
                QElapsedTimer opening;opening.start();
                while(!popup->property("opened").toBool() && opening.elapsed()<3000)settle(10);
                check(popup->property("opened").toBool(),"QML menu never fully opened");settle(100);report["popupOpen"]=true;
                auto* content=popup->property("contentItem").value<QQuickItem*>();check(content && content->parentItem(),"QML popup visual item absent");
                auto* popupItem=content->parentItem();const QRectF bounds(popupItem->mapToScene(QPointF()),QSizeF(popupItem->width(),popupItem->height()));
                const auto pixels=settings->grabFramebuffer();const auto visible=bounds.intersected(QRectF(settings->rect()));
                report["popup"]=QJsonObject{{"bounds",rect(bounds.translated(settings->mapTo(&panel,QPoint())))},{"visibleBounds",rect(visible.translated(settings->mapTo(&panel,QPoint())))},{"rows",combo->property("count").toInt()},{"font",combo->property("font").value<QFont>().toString()}};
                const int rows=combo->property("count").toInt();
                const auto selected=content->mapToScene(QPointF(4,(combo->property("currentIndex").toInt()+.5)*content->height()/rows));
                const auto background=pixels.pixelColor(int(selected.x()*pixels.width()/settings->width()),int(selected.y()*pixels.height()/settings->height()));
                auto popupReport=report["popup"].toObject();
                popupReport["selectedBackground"]=background.name();
                popupReport["themeHighlight"]=ui::Theme::instance().palette().color(QPalette::Active,QPalette::Highlight).name();
                report["popup"]=popupReport;
                check(pixels.copy(QRect(int(visible.x()*pixels.width()/settings->width()),int(visible.y()*pixels.height()/settings->height()),int(visible.width()*pixels.width()/settings->width()),int(visible.height()*pixels.height()/settings->height()))).save(artifacts.filePath(name+"-menu.png")),"save QML popup framebuffer");QMetaObject::invokeMethod(popup,"close");
            }
#else
            QComboBox* combo=nullptr;for(auto* candidate:panel.findChildren<QComboBox*>("numericDisplayMode"))if(candidate->isVisible())combo=candidate;
            check(combo,"visible Widgets mode combo absent");
            if(combo->isEnabled()){
                auto* popup=combo->view()->window();popup->setAttribute(Qt::WA_ShowWithoutActivating);popup->setWindowFlag(Qt::WindowDoesNotAcceptFocus,true);
                combo->showPopup();settle(100);check(popup->isVisible(),"Widgets popup not open");report["popupOpen"]=true;
                report["popup"]=QJsonObject{{"bounds",rect(QRectF(panel.mapFromGlobal(popup->mapToGlobal(QPoint())),popup->size()))},{"rows",combo->count()},{"font",combo->font().toString()}};
                check(popup->grab().save(artifacts.filePath(name+"-menu.png")),"save Widgets popup framebuffer");combo->hidePopup();
            }
#endif
            cases.append(report);panel.hide();
        }
        QFile file(artifacts.filePath("manifest.json"));check(file.open(QIODevice::WriteOnly),"manifest output");file.write(QJsonDocument(QJsonObject{{"cases",cases},{"qtVersion",qVersion()},{"font",app.font().toString()},{"platform",app.platformName()}}).toJson());
        std::printf("Captured %lld graph cases, actual menus and layout telemetry\n",(long long)cases.size());return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"visual comparison capture FAIL: %s\n",error.what());return 1;}
}
