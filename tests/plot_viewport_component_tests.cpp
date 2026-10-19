#include <visualization/plot_viewport.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
void check(bool ok,const char* reason) {
    if(!ok) {std::fprintf(stderr,"Plot viewport: %s\n",reason);std::exit(1);}
}
bool near(double a,double b) {return std::abs(a-b)<1e-12;}
bool same(QRectF a,QRectF b) {
    return near(a.left(),b.left()) && near(a.top(),b.top()) &&
           near(a.width(),b.width()) && near(a.height(),b.height());
}
void check_bounds(QRectF view) {
    check(view.left()>=-1e-12 && view.top()>=-1e-12 && view.right()<=1+1e-12 && view.bottom()<=1+1e-12 &&
          view.width()>0 && view.height()>0,"crop escaped the configured range");
}
void anchored_zoom() {
    const QRectF original(.2,.1,.6,.7);
    const double x=.35,y=.6;
    const auto zoomed=navigatePlotViewport(original,"zoom",x,y,.5,3);
    check(near(original.left()+x*original.width(),zoomed.left()+x*zoomed.width()) &&
          near(original.top()+y*original.height(),zoomed.top()+y*zoomed.height()),
          "zoom moved the selected source coordinate");
    check(zoomed.width()<original.width() && zoomed.height()<original.height(),"zoom did not crop both axes");
    check(same(navigatePlotViewport(zoomed,"zoom",x,y,2,3),original),"inverse zoom changed the source range");
    check_bounds(zoomed);
}
void pan_and_axis_mask() {
    const QRectF original(.2,.1,.6,.7);
    const auto x_only=navigatePlotViewport(original,"zoom",.5,.5,.5,1);
    check(near(x_only.top(),original.top()) && near(x_only.height(),original.height()),"X-only zoom changed Y");
    const auto moved=navigatePlotViewport(x_only,"pan",.1,-.1,1,3);
    check(moved.left()<x_only.left() && moved.top()>x_only.top(),"pan direction ignored visible-axis fractions");
    check(same(navigatePlotViewport(moved,"pan",-.1,.1,1,3),x_only),"opposing pans changed the crop");
    const auto reversed=navigatePlotViewport(original,"pan",.1,.1,1,3,true,true);
    check(reversed.left()>original.left() && reversed.top()>original.top(),"reversed axes ignored pan direction");
}
void refusal_and_limits() {
    const QRectF original(.2,.1,.6,.7);
    const auto far=navigatePlotViewport(original,"pan",100,-100,1,3);
    check(near(far.left(),0) && near(far.bottom(),1),"pan did not stop at configured bounds");
    check_bounds(far);
    auto tiny=original;
    for(int i=0;i<8;++i)tiny=navigatePlotViewport(tiny,"zoom",0,0,.1,3);
    check_bounds(tiny);
    check(tiny.width()>=1e-6 && tiny.height()>=1e-6,"zoom exceeded minimum crop");
    const auto nan=std::numeric_limits<double>::quiet_NaN();
    check(same(navigatePlotViewport(original,"zoom",nan,.5,.5,3),original),"nonfinite input changed crop");
    check(same(navigatePlotViewport(original,"unknown",.5,.5,.5,3),original),"unknown action changed crop");
    check(same(navigatePlotViewport(tiny,"reset",0,0,1,3),QRectF(0,0,1,1)),"reset did not restore full range");
}
}
int main() {
    anchored_zoom();pan_and_axis_mask();refusal_and_limits();
    return 0;
}
