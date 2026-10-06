#include "visualization/spectrograms/spectrogram_scene_geometry.h"
#include "visualization/surface_settings.h"
#include "visualization/slice_overlay.h"
#include <cstdio>
#include <cmath>

// Pure numerical contracts: no QApplication, window, GL context or timers.
int main() {
    auto check=[](bool condition,const char *message) {
        if(!condition) std::fprintf(stderr,"FAIL: %s\n",message);
        return condition;
    };
    const int full[4]={0,100,0,100},crop[4]={25,75,0,100};
    SliceOverlay overlay;
    const QSizeF area(800,600);
    const auto initial=overlay.rect(area);
    if(!check(overlay.begin(area,initial.topLeft()+QPointF(1,1)),"Resize corner hit")) return 1;
    overlay.move(area,initial.topLeft()-QPointF(200,120));overlay.end();
    const auto enlarged=overlay.rect(area);
    if(!check(enlarged.width()>initial.width() && enlarged.height()>initial.height() &&
              QRectF(QPointF(),area).contains(enlarged),"Resize grows and stays in viewport")) return 1;
    overlay.begin(area,enlarged.topLeft()+QPointF(1,1));
    overlay.move(area,enlarged.bottomRight()+QPointF(400,400));overlay.end();
    if(!check(overlay.rect(area).width()>=180 && overlay.rect(area).height()>=120,
              "Resizing cannot collapse the slice axes")) return 1;
    if(!check(QRectF(0,0,100,80).contains(overlay.rect({100,80})),"Small viewport clips preferred size")) return 1;
    for (const QSizeF viewport : {QSizeF(80, 60), QSizeF(180, 100), QSizeF(800, 600)})
      for(const QPointF position:{QPointF(0,0),QPointF(.5,.5),QPointF(1,1),QPointF(-1,2)}) {
        const auto panel = spectrogramSlicePanel(viewport,position), plot = spectrogramSlicePlot(panel);
        if (!check(QRectF(QPointF(0, 0), viewport).contains(panel) && panel.contains(plot) &&
                   plot.width() > 0 && plot.height() > 0,
                   "Slice preview and plot remain inside a small viewport")) return 1;
    }
    for(bool reverseTime:{false,true}) for(bool reverseBands:{false,true}) for(bool reverseAmplitude:{false,true}) {
        for(const auto *ranges:{full,crop}) {
            const SpectrogramSceneGeometry scene(7,3,ranges,reverseTime,reverseBands,0,2,false,reverseAmplitude);
            const auto left=scene.point(scene.timeTick(0),scene.zMin,1);
            const auto right=scene.point(scene.timeTick(1),scene.zMin,1);
            const auto front=scene.point(scene.xMin,scene.bandTick(0),1);
            const auto back=scene.point(scene.xMin,scene.bandTick(1),1);
            if(!check(std::abs(left.x()+scene.scaleX)<1e-5 && std::abs(right.x()-scene.scaleX)<1e-5,
                      "Time tick values must match the actual mirrored mesh endpoints") ||
               !check(std::abs(front.z()-scene.scaleZ)<1e-5 && std::abs(back.z()+scene.scaleZ)<1e-5,
                      "Band tick values must match the actual mirrored mesh endpoints") ||
               !check(left.y()==0 && front.y()==0,"Mirroring cannot alter amplitude")) return 1;
            if(!check(std::abs(scene.point(2,1,scene.signalTick(0)).y()+1)<1e-5 &&
                      std::abs(scene.point(2,1,scene.signalTick(1)).y()-1)<1e-5 &&
                      scene.verticalFraction(scene.signalTick(0))==0 && scene.verticalFraction(scene.signalTick(1))==1,
                      "Reverse Y changes real mesh, ticks and slice coordinates together")) return 1;
            const auto origin=scene.point(2,1,1),dx=scene.point(3,1,1)-origin,dz=scene.point(2,2,1)-origin;
            auto normal=QVector3D::crossProduct(dx,dz).normalized();
            if(reverseTime!=reverseBands) normal=-normal;
            if(!check(normal.y()>.99f,"All mirror combinations must preserve upward surface lighting")) return 1;
        }
    }
    std::vector<std::vector<float>> rows{{.2f,.5f,.9f}};
    if(!check(spectrogramHistorySample(rows,0,0,7)==.9f && spectrogramHistorySample(rows,0,2,7)==.2f &&
              spectrogramHistorySample(rows,0,3,7)==0,"Newest age0, old placeholders zero")) return 1;
    rows[0].push_back(1.2f);
    if(!check(spectrogramHistorySample(rows,0,0,3)==1.2f && spectrogramHistorySample(rows,0,1,3)==.9f &&
              spectrogramHistorySample(rows,0,3,3)==0,"Appending advances age and overflow preserves newest samples")) return 1;
    const SpectrogramSceneGeometry manual(7,3,full,false,false,-2,6,true);
    if(!check(manual.point(2,1,-2).y()==-1 && manual.point(2,1,6).y()==1 &&
              manual.point(2,1,2).y()==0 && manual.point(2,1,20).y()==1 &&
              manual.signalTick(.25f)==0 && manual.valueFraction(2)==.5f,
              "Manual amplitude range maps mesh, legend and slice consistently with visual clipping")) return 1;
    const auto raw=rows;
    (void)manual.point(0,0,spectrogramHistorySample(rows,0,0,3));
    if(!check(rows==raw,"Presentation scaling cannot mutate raw history")) return 1;
    if(!check(spectrogramTextColor(QColor(255,192,203))==QColor(Qt::black) &&
              spectrogramTextColor(QColor(24,24,24))==QColor(Qt::white),
              "Scene text contrasts with actual custom pink and dark backgrounds")) return 1;
    const QRectF viewport(0,0,640,480),ticks(280,290,120,36);
    const auto title=spectrogramAxisTitleRect({320,300},{0,1},{160,24},viewport,{ticks});
    if(!check(viewport.contains(title) && !title.intersects(ticks),
              "Projected axis titles stay within the scene and avoid numeric labels")) return 1;
    using namespace SurfaceSettings;
    if(!check(renderFps({},144)==60 && renderFps(QJsonObject{{"gui_fps_mode","screen"}},144)==144 &&
              renderFps(QJsonObject{{"gui_fps_mode","screen"}},0)==60 &&
              renderFps(QJsonObject{{"gui_target_fps",999}})==240,
              "GUI cadence defaults, screen selection, fallback and bounds")) return 1;
    const auto base=defaults();
    if(!check(base["axis_grid"].toBool() && base["floor_grid"].toBool() &&
              base["axis_precision"].toInt()==0 && !base["reverse_bands"].toBool(),
              "Space grid and axis defaults")) return 1;
    if(!check(normalized(QJsonObject{{"axis_precision",8}})["axis_precision"].toInt()==8 &&
              normalized(QJsonObject{{"axis_precision",99}})["axis_precision"].toInt()==8,
              "Eight axis decimals are supported and bounded")) return 1;
    const auto linear=gradientStops(QJsonObject{{"gradient","green_red"},{"gradient_layout","linear"}});
    const auto logarithmic=gradientStops(QJsonObject{{"gradient","green_red"},{"gradient_layout","log"}});
    if(!check(linear.size()==4 && std::abs(linear[1].first-1.0/3)<1e-8 &&
              logarithmic[1].first>linear[1].first && logarithmic.back().first==1,
              "Linear/log color transitions")) return 1;
    QJsonArray custom{QJsonObject{{"position",0},{"color","#123456"},{"extension",99}},
                      QJsonObject{{"position",1},{"color","#abcdef"}}};
    const auto value=normalized(QJsonObject{{"gradient_stops",custom},{"gradient_layout","log"},{"extra",42}});
    if(!check(value["extra"].toInt()==42 && value["gradient_stops"].toArray()[0].toObject()["extension"].toInt()==99 &&
              gradientStops(value)[0].second==QColor("#123456"),
              "Custom color stops and unknown metadata survive normalization")) return 1;
    std::puts("PASS: reverse X/Y/Z numeric mesh/ticks/crop/slice/normals; newest age0, aging, padding, overflow");
    return 0;
}
