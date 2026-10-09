#include <visualization/spectrograms/spectrogram_graph.h>
#include <visualization/axis_tick_labels.h>
#include <visualization/spectrograms/spectrogram_scene_geometry.h>
#include <cstdio>
int main() {
    int failures=0,checks=0;
    const auto check=[&](bool value){++checks;if(!value)++failures;};
    SpectrogramAxisMetadata axes;axes.time_coordinates=axes.column_coordinates=true;
    axes.ages={0,.01,.035};axes.columns={70,100,180};
    check(axes.age(0)==0);check(axes.age(2)==.035);check(axes.age(.5)==.005);
    check(axes.column(0)==70);check(axes.column(1.5)==140);
    check(std::isnan(axes.age(3)));check(std::isnan(axes.column(3)));
    check(std::isnan(axes.age(-1)));check(std::isnan(axes.age(std::numeric_limits<double>::infinity())));
    axes.time_coordinates=axes.column_coordinates=false;
    check(axes.age(0)==0);check(axes.age(8)==8);check(axes.column(4)==4);
    check(axes==axes);
    check(spectrogramAutomaticRange({})==std::pair<float,float>{0,1});
    check(spectrogramAutomaticRange({{0,0}})==std::pair<float,float>{0,1});
    check(spectrogramAutomaticRange({{0,.2f,.8f}})==std::pair<float,float>{0,.8f});
    check(spectrogramAutomaticRange({{-3,.5f}})==std::pair<float,float>{-3,.5f});
    const QLocale en("en_US"),ru("ru_RU");
    const int ranges[4]={0,100,0,100};
    SpectrogramSceneGeometry scene(8,50,ranges);
    std::vector<double> y,x;
    for(int i=0;i<=5;++i) {y.push_back(scene.signalTick(i/5.f));x.push_back(scene.timeTick(i/5.f));}
    check(axisTickLabels(y,en)==QStringList({"0.0","0.4","0.8","1.2","1.6","2.0"}));
    check(axisTickLabels(x,en)==QStringList({"0.0","1.4","2.8","4.2","5.6","7.0"}));
    check(axisTickLabels(y,ru)[1]=="0,4");
    check(axisTickLabels({0,.5,1,1.5,2},en)[1]=="0.5");
    check(axisTickLabels({-2,-1,0,1,2},en)==QStringList({"-2","-1","0","1","2"}));
    check(axisTickLabels({0,10,20,30,40,50},en)[1]=="10");
    for(const auto &ticks:std::vector<std::vector<double>>{{0,2e-8,4e-8},{-1e8,0,1e8},{1e8,1e8+.25,1e8+.5},{.005,.01,.02}}) {
        const auto labels=axisTickLabels(ticks,en);
        for(int i=0;i<labels.size();++i) {
            check(std::isfinite(en.toDouble(labels[i])));
            if(i) check(labels[i]!=labels[i-1]);
        }
    }
    check(axisTickLabels({-0.0,1,2},en,2)[0]=="0.00");
    check(axisTickLabels({0,std::numeric_limits<double>::quiet_NaN(),1},en)[1].isEmpty());
    // Frequency endpoints are sample centres, including a singleton column.
    axes.time_coordinates=axes.column_coordinates=true;
    axes.ages={0,.01,.035};axes.columns={70,100,180};
    SpectrogramSceneGeometry physical(400,3,ranges);
    check(physical.zMax==2 && axes.column(physical.bandTick(1))==180);
    const auto partial=spectrogramValidTimeTicks(physical,3);
    check(partial.front()==0 && partial.back()==2);
    for(double index:partial)check(std::isfinite(axes.age(index)));
    check(std::isnan(spectrogramValidTimeTicks(physical,0).front()));
    const int cropped[4]={25,75,0,100};
    check(std::isnan(spectrogramValidTimeTicks(SpectrogramSceneGeometry(400,3,cropped),3).front()));
    check(spectrogramValidTimeTicks(SpectrogramSceneGeometry(400,3,ranges,true),3).front()==2);
    SpectrogramSceneGeometry singleton(1,1,ranges);
    check(singleton.xMin==0 && singleton.xMax==0 && singleton.zMin==0 && singleton.zMax==0);
    check(singleton.point(0,0,1).x()==0 && singleton.point(0,0,1).z()==0);
    for(int precision=0;precision<=8;++precision)for(const QLocale& locale:{en,ru})
        for(double value:{.03,1./60.,-1e-8,2e8}) {
            const double displayed=locale.toDouble(axisReadoutLabel(value,locale,precision));
            check(displayed!=0 && std::abs(displayed-value)<=std::abs(value)*.01);
        }
    const QRectF viewport(4,4,792,592),tick(770,570,40,20);
    const auto placed=spectrogramTickLabelRect(tick,viewport,{});
    check(!placed.isEmpty() && viewport.contains(placed));
    check(spectrogramTickLabelRect(tick,viewport,{placed}).isEmpty());
    check(spectrogramTickLabelRect(QRectF(900,570,40,20),viewport,{}).isEmpty());
    std::printf("numeric spectrogram axes: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
