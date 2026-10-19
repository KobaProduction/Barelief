#include "visualization/spectrograms/spectrogram_scene_geometry.h"
#include "visualization/spectrograms/spectrogram_mesh_topology.h"
#include "visualization/axis_tick_labels.h"
#include <array>
#include <limits>
#include <utility>
#include "visualization/surface_settings.h"
#include "visualization/slice_overlay.h"
#include <QMatrix4x4>
#include <cstdio>
#include <cmath>

// Pure numerical contracts: no QApplication, window, GL context or timers.
int main() {
    auto check=[](bool condition,const char *message) {
        if(!condition) std::fprintf(stderr,"FAIL: %s\n",message);
        return condition;
    };
    // Actual regression: a fixed 399/120-second span acquired +/-ULP noise
    // from absolute timestamp subtraction. Half-decimal interior ticks changed
    // strings every frame and forced complete 3D label-atlas uploads.
    for(const auto locale:{QLocale(QLocale::English),QLocale(QLocale::Russian)}) {
        std::vector<double> ticks;
        for(int i=0;i<=14;++i)ticks.push_back(3.325*i/14);
        const auto original=ticks;const auto expected=axisTickLabels(ticks,locale);
        for(double origin:{4.,7.,12.,16.}) {
            std::vector<double> fromTimestamp;
            for(double tick:ticks)fromTimestamp.push_back(origin-(origin-tick));
            if(!check(axisTickLabels(fromTimestamp,locale)==expected,
                      "Physical seconds keep stable labels after timestamp cancellation"))return 1;
        }
        if(!check(ticks==original,"Formatting never mutates physical coordinates"))return 1;
        for(double sign:{-1.,1.}) {
            const auto tied=axisTickLabels({0,sign*.2375,sign*.475},locale,3);
            if(!check(tied[1]==locale.toString(sign*.238,'f',3),"Half ties use deterministic symmetric rounding"))return 1;
            for(double noise:{-1e-15,1e-15})
                if(!check(axisTickLabels({0,sign*(.2375+noise),sign*.475},locale,3)==tied,
                          "Positive and negative decimal ties ignore cancellation noise"))return 1;
            const auto scientific=axisTickLabels({0,sign*2.375e-7,sign*4.75e-7},locale,2);
            for(double noise:{-1e-22,1e-22})
                if(!check(axisTickLabels({0,sign*(2.375e-7+noise),sign*4.75e-7},locale,2)==scientific,
                          "Scientific half ties retain stable labels for tiny coordinates"))return 1;
        }
        if(!check(axisTickLabels({0,.2375-1e-9,.475},locale,3)[1]!=
                  axisTickLabels({0,.2375+1e-9,.475},locale,3)[1],
                  "Actual movement across a decimal boundary remains visible"))return 1;
        if(!check(axisTickLabels({0,.2375,.475},locale,6)[1]==locale.toString(.2375,'f',6),
                  "Requested decimal precision is retained"))return 1;
        const auto close=axisTickLabels({1.,1.+1e-12,1.+2e-12},locale,15);
        if(!check(close[0]!=close[1] && close[1]!=close[2],
                  "Fine adjacent physical coordinates are not collapsed by tie stabilization"))return 1;
        if(!check(locale.toDouble(axisReadoutLabel(1e-12,locale))!=0,
                  "Small nonzero values do not become zero"))return 1;
    }
    // Independent expanded-mesh oracle: indexed draws must submit the exact
    // same positions AND normals, with the same winding and cell diagonal.
    // This also preserves interpolated lighting, picking and shadow coverage.
    for(const auto dimensions : {std::pair<size_t,size_t>{0,0}, {0,7}, {7,0},
                                 {1,1}, {1,5}, {5,1}, {2,2}, {4,7}, {50,400}}) {
        const auto [meshRows,meshColumns]=dimensions;
        std::vector<std::uint32_t> indices;
        if(!check(spectrogramMeshTopology(meshRows,meshColumns,indices),"Valid topology admitted"))return 1;
        std::vector<std::array<float,6>> points(meshRows*meshColumns),expanded;
        for(size_t row=0;row<meshRows;++row)for(size_t column=0;column<meshColumns;++column) {
            const float x=float(column),z=float(row),y=float(std::sin(x*.3)*std::cos(z*.2));
            const auto normal=QVector3D(-.3f*std::cos(x*.3f)*std::cos(z*.2f),1,
                                       .2f*std::sin(x*.3f)*std::sin(z*.2f)).normalized();
            points[row*meshColumns+column]={x,y,z,normal.x(),normal.y(),normal.z()};
        }
        auto appendPoint=[&](size_t row,size_t column){expanded.push_back(points[row*meshColumns+column]);};
        if(meshRows>1 && meshColumns>1) {
            for(size_t row=0;row+1<meshRows;++row)for(size_t column=0;column+1<meshColumns;++column) {
                appendPoint(row,column);appendPoint(row+1,column);appendPoint(row,column+1);
                appendPoint(row,column+1);appendPoint(row+1,column);appendPoint(row+1,column+1);
            }
        } else if(meshRows && meshColumns) {
            if(meshRows*meshColumns==1)appendPoint(0,0);
            else if(meshRows==1)for(size_t c=0;c+1<meshColumns;++c){appendPoint(0,c);appendPoint(0,c+1);}
            else for(size_t r=0;r+1<meshRows;++r){appendPoint(r,0);appendPoint(r+1,0);}
        }
        if(!check(indices.size()==expanded.size(),"Indexed draw preserves submitted element count"))return 1;
        for(size_t i=0;i<indices.size();++i)
            if(!check(indices[i]<points.size() && points[indices[i]]==expanded[i],
                      "Indexed triangles/lines/points preserve exact attributes and winding"))return 1;
        // Crease construction consumes triangle positions in draw order. Its
        // emitted endpoints must still address exactly those physical points.
        auto position=[](const std::array<float,6>& p){return QVector3D(p[0],p[1],p[2]);};
        for(size_t cell=0;meshRows>1 && meshColumns>1 && cell<indices.size()/6;++cell) {
            const auto v=cell*6;
            const auto a=QVector3D::crossProduct(position(expanded[v+1])-position(expanded[v]),
                                                position(expanded[v+2])-position(expanded[v])).normalized();
            const auto b=QVector3D::crossProduct(position(points[indices[v+1]])-position(points[indices[v]]),
                                                position(points[indices[v+2]])-position(points[indices[v]])).normalized();
            if(!check(a==b && position(points[indices[v+3]])==position(expanded[v+3]) &&
                      position(points[indices[v+4]])==position(expanded[v+4]),
                      "Crease face normals and diagonal endpoints remain exact"))return 1;
        }
        if(meshRows==50 && meshColumns==400 && !check(points.size()*6*sizeof(float)==480000 &&
            expanded.size()*6*sizeof(float)==2815344 && indices.size()*sizeof(std::uint32_t)==469224,
            "400x50 keeps exact mesh with smaller dynamic vertex payload"))return 1;
    }
    std::vector<std::uint32_t> rejected{17,23};
    if(!check(!spectrogramMeshTopology(std::numeric_limits<size_t>::max(),2,rejected) &&
              rejected==std::vector<std::uint32_t>{17,23},"Overflow rejected before mutating topology"))return 1;
    const int full[4]={0,100,0,100},crop[4]={25,75,0,100};
    // Cached and direct positions/normals remain bit-identical, including crop
    // neighbours, reversed axes and clipped signed amplitudes.
    const auto valueAt=[](size_t band,size_t age){return float(std::sin(double(age)*.17)+std::cos(double(band)*.21));};
    SpectrogramPointCache pointCache;
    for(bool reverseTime:{false,true})for(bool reverseBands:{false,true})for(bool reverseAmplitude:{false,true}){
        SpectrogramSceneGeometry scene(23,17,crop,reverseTime,reverseBands,-.5f,1.3f,true,reverseAmplitude);
        pointCache.prepare(scene,3,14,5,20,17,23,valueAt);
        if(!check(pointCache.enabled(),"Small projection cache enabled"))return 1;
        for(size_t band=3;band<14;++band)for(size_t age=5;age<20;++age){
            const auto direct=scene.point(age,band,valueAt(band,age));
            const auto cached=pointCache.point(scene,age,band,valueAt(band,age));
            const auto dx=scene.point(age+1,band,valueAt(band,age+1))-scene.point(age-1,band,valueAt(band,age-1));
            const auto dz=scene.point(age,band+1,valueAt(band+1,age))-scene.point(age,band-1,valueAt(band-1,age));
            const auto cachedDx=pointCache.point(scene,age+1,band,valueAt(band,age+1))-pointCache.point(scene,age-1,band,valueAt(band,age-1));
            const auto cachedDz=pointCache.point(scene,age,band+1,valueAt(band+1,age))-pointCache.point(scene,age,band-1,valueAt(band-1,age));
            if(!check(direct==cached && QVector3D::crossProduct(dx,dz).normalized()==QVector3D::crossProduct(cachedDx,cachedDz).normalized(),
                      "Projection cache preserves exact positions and normals"))return 1;
        }
    }
    SpectrogramSceneGeometry hugeScene(16384,4096,full);
    pointCache.prepare(hugeScene,0,4096,0,16384,4096,16384,valueAt);
    if(!check(!pointCache.enabled() && pointCache.point(hugeScene,5,3,valueAt(3,5))==hugeScene.point(5,3,valueAt(3,5)),
              "Oversized projection cache falls back without changing geometry"))return 1;
    const QSizeF tickViewport(1200,800),labelSize(40,18);
    const int compactTicks=spectrogramAxisSections({320,200},labelSize,0);
    const int largeTicks=spectrogramAxisSections(tickViewport,labelSize,0);
    if(!check(largeTicks>5 && largeTicks>compactTicks,
              "A larger viewport gets more divisions independent of camera projection"))return 1;
    if(!check(spectrogramAxisSections(tickViewport,{100,18},0)<largeTicks,
              "Long coordinate labels reduce layout density"))return 1;
    if(!check(spectrogramAxisSections({800,220},{400,40},2)>=4,
              "A usable viewport retains horizontal amplitude grid divisions"))return 1;
    if(!check(spectrogramAxisSections({100,80},labelSize,2)>=1 &&
              spectrogramAxisSections({100000,100000},{1,1},0)<=64,
              "Tiny and large layouts have bounded density"))return 1;
    const int stableTime=spectrogramAxisSections(tickViewport,labelSize,0),
              stableBands=spectrogramAxisSections(tickViewport,labelSize,1),
              stableAmplitude=spectrogramAxisSections(tickViewport,labelSize,2);
    for(float yaw:{-179.f,-90.f,0.f,90.f,179.f})for(float pitch:{-89.f,0.f,89.f})for(float distance:{.5f,3.f,10.f}) {
        QMatrix4x4 camera;camera.perspective(45,1.5f,.005f,100);camera.translate(0,0,-distance);
        camera.rotate(pitch,1,0,0);camera.rotate(yaw,0,1,0);
        // Deliberately exercise edge-on/behind-camera projections. The actual
        // renderer policy takes layout inputs only; none of these alter ticks.
        const auto projected=camera*QVector4D(2,1,1,1);(void)projected;
        if(!check(spectrogramAxisSections(tickViewport,labelSize,0)==stableTime &&
                  spectrogramAxisSections(tickViewport,labelSize,1)==stableBands &&
                  spectrogramAxisSections(tickViewport,labelSize,2)==stableAmplitude,
                  "Camera yaw, pitch, zoom and culling cannot change divisions"))return 1;
    }
    if(!check(spectrogramReadableSections(399,20,true)==20 && spectrogramReadableSections(397,20,true)==20 &&
              spectrogramReadableSections(400,20,true)==20 &&
              spectrogramReadableSections(2,7,false)==4 &&
              spectrogramReadableSections(2.6,20,false)==13,
              "Frame ticks stay integral and exact amplitude endpoints permit readable steps"))return 1;
    for(bool reversed:{false,true}) {
        const SpectrogramSceneGeometry frameScene(398,50,full,reversed);
        const auto frameTicks=spectrogramValidTimeTicks(frameScene,398,20,true);
        if(!check(frameTicks.size()==21 && frameTicks.front()==frameScene.timeTick(0) && frameTicks.back()==frameScene.timeTick(1),
                  "Prime frame capacities preserve adaptive density and exact endpoints"))return 1;
        for(size_t index=1;index+1<frameTicks.size();++index)
            if(!check(frameTicks[index]==std::round(frameTicks[index]),"Temporal frame positions are integral"))return 1;
        const SpectrogramSceneGeometry scene(400,50,crop,reversed);
        const auto adaptive=spectrogramValidTimeTicks(scene,400,largeTicks);
        if(!check(adaptive.size()==size_t(largeTicks+1) &&
                  adaptive.front()==scene.timeTick(0) && adaptive.back()==scene.timeTick(1),
                  "Adaptive tick counts preserve crop and reversed exact endpoints"))return 1;
        for(size_t index=1;index<adaptive.size();++index)
            if(!check(reversed ? adaptive[index]<adaptive[index-1] : adaptive[index]>adaptive[index-1],
                      "Every adaptive tick follows the same coordinate direction"))return 1;
    }
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
    // Both slice planes follow the projected axis, rather than assuming that
    // increasing bands (negative world Z) run rightward on screen.
    for(bool reverseTime:{false,true})for(bool reverseBands:{false,true})
      for(float yaw:{-130.f,-30.f,45.f,140.f})for(bool timeSlice:{false,true})for(const auto* ranges:{full,crop}) {
        const SpectrogramSceneGeometry scene(24,9,ranges,reverseTime,reverseBands);
        QMatrix4x4 camera;camera.perspective(45,1.5f,.005f,100);camera.translate(0,0,-8);
        camera.rotate(35,1,0,0);camera.rotate(yaw,0,1,0);
        const auto point=[&](int index){return camera*QVector4D(scene.point(timeSlice?8:index,
            timeSlice?index:4,scene.signalTick(0)),1);};
        const auto first=point(int(std::ceil(timeSlice?scene.zMin:scene.xMin))),
                   last=point(int(std::floor(timeSlice?scene.zMax:scene.xMax)));
        const bool reverse=spectrogramSliceReversed(first,last);
        const auto left=reverse?last:first,right=reverse?first:last;
        if(!check(left.x()/left.w()<=right.x()/right.w()+1e-6,
                  "Both slice modes follow screen-left to screen-right under camera and reverse axes"))return 1;
      }
    if(!check(!spectrogramSliceReversed({0,1,0,1},{0,-1,0,1}) &&
               spectrogramSliceReversed({0,-1,0,1},{0,1,0,1}) &&
               !spectrogramSliceReversed({0,0,0,1},{0,0,0,1}),
               "Edge-on slice axes follow top-to-bottom, coincident endpoints preserve source order"))return 1;
    const int onePercent[4]={0,1,0,100},twoPercent[4]={0,2,0,100};
    for(const auto* ranges:{full,onePercent,twoPercent})for(size_t frames:{1u,3u,400u,800u})for(size_t bands:{1u,50u,2048u}) {
        const SpectrogramSceneGeometry stable(frames,bands,ranges);
        if(!check(stable.scaleX==2 && stable.scaleZ==1,
            "Fixed scene proportions must not depend on history, bands or crop"))return 1;
        const SpectrogramSceneGeometry custom(frames,bands,ranges,false,false,0,2,false,false,false,3,.75);
        if(!check(custom.scaleX==3 && custom.scaleZ==.75f,
            "Custom visual ratios remain independent of coordinate units"))return 1;
    }
    const SpectrogramSceneGeometry automatic(400,50,onePercent,false,false,0,2,false,false,true);
    if(!check(std::abs(automatic.scaleX-2.f*3.99f/49.f)<1e-6f && automatic.scaleZ==2,
        "Explicit sample-count mode preserves its crop-dependent historical policy"))return 1;
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
    std::vector<std::vector<float>> narrow(2048,std::vector<float>(8,0));
    narrow[3][4]=.9f; // a uniform stride of eight used to drop this peak
    const auto peak=spectrogramPeakSample(narrow,0,8,0,8,8);
    if(!check(peak.band==3 && peak.age==3 && peak.value==.9f,
              "Decimation retains an off-stride narrow frequency/time peak"))return 1;
    narrow[6][6]=-1.2f;
    const auto signedPeak=spectrogramPeakSample(narrow,0,8,0,8,8);
    if(!check(signedPeak.band==6 && signedPeak.age==1 && signedPeak.value==-1.2f,
              "Signed decimation retains the largest absolute excursion"))return 1;
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
    if(!check(renderFps({},360)==360 && renderFps(QJsonObject{{"gui_fps_mode","screen"}},144)==144 &&
              renderFps(QJsonObject{{"gui_fps_mode","screen"}},0)==60 &&
              renderFps(QJsonObject{{"gui_target_fps",999}})==999 && renderFps(QJsonObject{{"gui_target_fps",9999}})==1000,
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
