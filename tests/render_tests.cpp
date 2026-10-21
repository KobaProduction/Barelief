#include "isolated_gui_platform.h"
#include "numeric_render_fixture.h"
#include "numeric_gui_fixture.h"
#include <core_app/numeric_view_adapter.h>
#include <barelief/spectrograms/spectrogram_graph.h>
#include <barelief/ui_text.h>
#include <barelief/numeric_image_view.h>
#include <cstdio>
static void shared_projection_and_reset(auto& application,auto& adapter,auto& ctx,auto& surface,auto& target,auto& marker,float& sample) {
using namespace gui_fixture;
        NumericImageView image;
        check(adapter.render(ctx->endpoint,0,render_fixture::Target{nullptr,nullptr,nullptr,true,&image})==signal_contracts::Status::ok,"shared 2D history render");
        const auto* storage=surface.projection_storage_identity();
        check(storage && storage==image.projection_storage_identity(),"2D/3D History must share immutable projection");
        adapter.render(ctx->endpoint,0,target);
        check(surface.projection_storage_identity()==storage,"unchanged History must reuse projection");
        const auto revision=surface.history_projection_revision();
        ui::Theme::instance().set("light");application.processEvents();ui::Theme::instance().set("dark");application.processEvents();
        check(surface.history_projection_revision()==revision,"theme changed retained projection");
        auto settings=surface.surface_settings();settings["reverse_time"]=true;surface.set_surface_settings(settings);
        check(surface.projected_history_sample(0,0,sample) && sample==6,"axis direction rewrote data order");
        marker.compact.delivery={5,7};check(adapter.accept_history_snapshot(full_history(ctx,{5,7},0,0),0,0)==signal_contracts::Status::ok,"current authoritative reset snapshot");adapter.render(ctx->endpoint,0,target);
        check(surface.projected_history_frames()==400 && surface.projected_history_capacity()==400 && surface.projected_axis_metadata().ages.size()==400 && !surface.projected_axis_metadata().time_coordinates,"current reset must retain geometry and full frame axis");
        for(size_t frame=0;frame<400;++frame)for(size_t band=0;band<50;++band)
            check(surface.projected_history_sample(frame,band,sample) && sample==0,"current reset retained stale values");
}
static void partial_history_samples(SpectrogramGraph& surface,float& sample) {
    using namespace gui_fixture;
        for(size_t frame=0;frame<380;++frame)for(size_t band=0;band<50;++band)
            check(surface.projected_history_sample(frame,band,sample) && sample==0,"unfilled old tail must be zero");
        check(surface.projected_history_sample(380,0,sample) && sample==1,"oldest real frame ordering");
        check(surface.projected_history_sample(399,49,sample) && sample==20,"newest frame ordering");

}
int main(int argc,char** argv) {
    QApplication application(argc,argv);
    try {
        using namespace gui_fixture;
        check(test_fixture::isolated_gui_platform(),"isolated offscreen test required");
        const auto graph=numeric_test_graph();auto ctx=context(graph,8,domain::PortDirection::Input);
        presentation::NumericViewAdapter adapter({4,{400,50,1048576,{true,32768}},{true,32768}});check(adapter.bind(ctx->endpoint)==signal_contracts::Status::ok,"bind History input");
        SpectrogramGraph surface;QLabel label;
        render_fixture::Target target{nullptr,&surface,&label,false};
        for(unsigned i=1;i<=20;++i)check(adapter.accept_history_snapshot(full_history(ctx,{3,7},i,float(i)),i,std::min(std::uint64_t(i),std::uint64_t(400)))==signal_contracts::Status::ok,"accept hidden row");
        check(adapter.render(ctx->endpoint,0,target)==signal_contracts::Status::ok,"hidden render");
        check(surface.projected_history_frames()==0,"hidden view projected data");
        target.visible=true;check(adapter.render(ctx->endpoint,0,target)==signal_contracts::Status::ok,"reopen projection");
        check(surface.projected_history_frames()==400 && surface.projected_history_capacity()==400 && surface.projected_history_bands()==50,"fixed-capacity frame/band projection dimensions");
        check(!surface.projected_axis_metadata().time_coordinates && surface.projected_axis_metadata().ages.size()==400 && surface.projected_axis_metadata().ages.back()==399,"partial occupancy must retain full frame-age axis without invented times");
        float sample=0;
        partial_history_samples(surface,sample);
        for(unsigned i=21;i<=25;++i)adapter.accept_history_snapshot(full_history(ctx,{4,7},i,float(i)),i,std::min(std::uint64_t(i),std::uint64_t(400)));
        adapter.render(ctx->endpoint,0,target);check(surface.projected_history_frames()==400 && surface.projected_axis_metadata().ages.size()==400 && label.text().contains("25"),"compatible revision discarded accumulation");
        check(surface.projected_history_sample(375,0,sample) && sample==1 && surface.projected_history_sample(399,49,sample) && sample==25,"compatible revision changed real suffix ordering");
        pipeline::DeliveredNumericHistoryRow marker;marker.compact.context=ctx;marker.compact.delivery={3,7};marker.capacity=400;marker.event=pipeline::NumericHistoryEvent::Reset;
        adapter.accept(marker);adapter.render(ctx->endpoint,0,target);check(surface.projected_history_frames()==400 && surface.projected_axis_metadata().ages.size()==400 && label.text().contains("25"),"stale reset cleared new revision");
        for(unsigned i=26;i<=405;++i)adapter.accept_history_snapshot(full_history(ctx,{4,7},i,float(i)),i,std::min(std::uint64_t(i),std::uint64_t(400)));
        adapter.render(ctx->endpoint,0,target);check(surface.projected_history_frames()==400,"bounded History size");
        check(surface.projected_history_sample(0,0,sample) && sample==6,"ring oldest row");
        check(surface.projected_history_sample(399,49,sample) && sample==405,"ring newest row");
        shared_projection_and_reset(application,adapter,ctx,surface,target,marker,sample);
        std::puts("Canonical retained History projection PASS; native OpenGL pixels are outside this offscreen test");return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"render: %s\n",error.what());return 1;}
}
