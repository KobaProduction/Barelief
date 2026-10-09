#include <pipeline/numeric_engine.h>
#include <pipeline/test_signal.h>
#include <core_app/numeric_default_graph.h>
#include <desktop/numeric_project_document.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QThread>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace signal_contracts;
using namespace pipeline;
namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool wait_for(const std::function<bool()>& predicate,int timeout=3000){
    QElapsedTimer timer;timer.start();while(!predicate() && timer.elapsed()<timeout){QCoreApplication::processEvents();QThread::msleep(1);}return predicate();
}
struct Signature {
    std::uint64_t ordinal=0,filled=0,producer=0,generation=0,sequence=0;
    std::size_t rows=0,discontinuities=0;
    std::uint64_t delivery_revision=0,delivery_epoch=0;
    std::size_t revision_boundaries=0,boundary_skipped_rows=0;
    bool nonzero=false,changed=false;
    std::array<float,50> latest{};
};
void print(const char* label,const Signature& s){
    std::printf("%s rows=%zu ordinal=%llu filled=%llu producer=%llu generation=%llu sequence=%llu discontinuities=%zu nonzero=%d changed=%d\n",label,s.rows,
        (unsigned long long)s.ordinal,(unsigned long long)s.filled,(unsigned long long)s.producer,
        (unsigned long long)s.generation,(unsigned long long)s.sequence,s.discontinuities,int(s.nonzero),int(s.changed));
}
}
int main(int argc,char** argv){QCoreApplication application(argc,argv);try{
    auto graph=numeric_test_graph();
    NumericEngine engine([](auto){return make_test_signal_source();});
    const EndpointIdentity producer{"main",7,domain::PortDirection::Output,0};
    const EndpointIdentity consumer{"main",8,domain::PortDirection::Input,0};
    const EndpointIdentity count{"main",7,domain::PortDirection::Output,1};
    const EndpointIdentity scalar{"main",12,domain::PortDirection::Input,0};
    require(engine.subscribe_numeric_frames({producer,consumer,count,scalar}),"subscribe actual History and count endpoints");
    Signature source,sink;std::uint64_t count_value=0,scalar_value=0;
    std::size_t generic_history=0,generic_errors=0,post_stop_rows=0,post_stop_scalars=0;
    bool bad=false,active=true;
    engine.set_numeric_history_handler([&](DeliveredNumericHistoryRow row){
        if(!row.compact.context)return;
        const auto& endpoint=row.compact.context->endpoint;
        auto* s=endpoint==producer?&source:endpoint==consumer?&sink:nullptr;if(!s)return;
        if(row.event!=NumericHistoryEvent::Row)return;
        if(!active){++post_stop_rows;return;}
        if(row.compact.status!=Status::ok || !row.compact.frame){bad=true;return;}
        const auto frame=row.compact.frame.view();const auto* numeric=std::get_if<NumericView>(&frame.payload);
        if(!numeric || numeric->contract->semantic!=Semantic::MelEnergy || numeric->actual!=std::array<std::size_t,2>{50,1} ||
            numeric->planes.size()!=1 || row.capacity!=400 || row.filled_count!=std::min(row.accepted_row_ordinal,std::uint64_t{400})){bad=true;return;}
        if(s->ordinal && frame.stamp.producer_epoch==s->producer && frame.stamp.generation==s->generation){
            if(row.compact.delivery.epoch==s->delivery_epoch &&
               row.compact.delivery.graph_revision==s->delivery_revision){
                if(row.accepted_row_ordinal!=s->ordinal+1 || frame.stamp.sequence!=s->sequence+1 || frame.stamp.discontinuity){
                    std::fprintf(stderr,"History ordinal gap within revision node%d old%llu new%llu revision%llu\n",endpoint.node,
                        (unsigned long long)s->ordinal,(unsigned long long)row.accepted_row_ordinal,
                        (unsigned long long)s->delivery_revision);bad=true;
                }
            }else{
                // Apply retires queued old-revision presentation events. The
                // retained DSP rows are independently checked by cold snapshots.
                if(row.compact.delivery.epoch!=s->delivery_epoch ||
                   row.compact.delivery.graph_revision<=s->delivery_revision ||
                   row.accepted_row_ordinal<=s->ordinal || frame.stamp.sequence<=s->sequence ||
                   frame.stamp.sequence-s->sequence!=row.accepted_row_ordinal-s->ordinal || frame.stamp.discontinuity)bad=true;
                else {++s->revision_boundaries;s->boundary_skipped_rows+=row.accepted_row_ordinal-s->ordinal-1;}
            }
        }
        std::array<float,50> values{};
        for(std::size_t band=0;band<50;++band){if(load_element(*numeric,{0,band,0},values[band])!=Status::ok || !std::isfinite(values[band]))bad=true;
            s->nonzero|=values[band]>0;}
        if(s->rows && values!=s->latest)s->changed=true;
        s->latest=values;s->ordinal=row.accepted_row_ordinal;s->filled=row.filled_count;
        s->discontinuities+=frame.stamp.discontinuity;
        s->producer=frame.stamp.producer_epoch;s->generation=frame.stamp.generation;s->sequence=frame.stamp.sequence;++s->rows;
        s->delivery_revision=row.compact.delivery.graph_revision;s->delivery_epoch=row.compact.delivery.epoch;
    });
    engine.set_numeric_frame_handler([&](DeliveredNumericFrame delivered){
        if(!delivered.context)return;
        const auto& endpoint=delivered.context->endpoint;
        if(endpoint==producer || endpoint==consumer){++generic_history;generic_errors+=delivered.status!=Status::ok;return;}
        if(!delivered.frame || delivered.status!=Status::ok)return;
        if(endpoint!=count && endpoint!=scalar)return;
        if(!active){++post_stop_scalars;return;}
        const auto frame=delivered.frame.view();const auto* numeric=std::get_if<NumericView>(&frame.payload);std::uint64_t value=0;
        if(!numeric || numeric->contract->dtype!=DType::UInt64 || numeric->contract->shape.rank!=Rank::Scalar ||
            load_element(*numeric,{0,0,0},value)!=Status::ok || value>400){bad=true;return;}
        (endpoint==count?count_value:scalar_value)=value;
    });
    bool accepted=false,rejected=false;engine.set_reply_handler([&](const EngineReply& reply){accepted=reply.accepted;rejected=!reply.accepted;});
    auto submit=[&](EngineCommand command){accepted=rejected=false;engine.submit(std::move(command));require(wait_for([&]{return accepted||rejected;})&&accepted,"engine command rejected");};
    auto apply=[&]{QByteArray bytes;require(bool(desktop::encode_numeric_graph(graph,bytes)),"encode actual default graph");
        EngineCommand command;command.kind=EngineCommand::ApplyGraph;command.graph=QJsonDocument::fromJson(bytes).object();submit(std::move(command));};
    apply();require(wait_for([&]{return sink.filled>=20 && count_value>=20 && scalar_value>=20;}),"History/count absent at actual provider consumer");
    require(!bad && source.nonzero && sink.nonzero && sink.changed,"invalid or constant actual compact History delivery");
    print("producer7.Output0",source);print("consumer8.Input0",sink);
    DeliveredNumericFrame snapshot;std::uint64_t before_ordinal=0,after_ordinal=0,before_filled=0,after_filled=0;
    require(engine.query_numeric(consumer,snapshot,&before_ordinal,&before_filled)==Status::ok,"consumer History snapshot unavailable");
    const auto before_rows=before_filled;require(std::get<NumericView>(snapshot.frame.view().payload).actual[0]==400,"cold snapshot must expose fixed capacity");snapshot={};
    require(engine.query_numeric(consumer,snapshot,&before_ordinal,&before_filled)==Status::ok,"snapshot first row");
    std::array<float,50> oldest{};const auto initial=std::get<NumericView>(snapshot.frame.view().payload);
    for(std::size_t band=0;band<50;++band)require(load_element(initial,{0,initial.actual[0]-before_filled,band},oldest[band])==Status::ok,"read oldest History row");snapshot={};
    QThread::msleep(300);
    require(engine.query_numeric(consumer,snapshot,&after_ordinal,&after_filled)==Status::ok,"consumer History snapshot after idle unavailable");
    const auto after_view=std::get<NumericView>(snapshot.frame.view().payload);
    require(after_view.contract->semantic==Semantic::History && after_view.actual[1]==50 && after_view.actual[0]==400 && after_filled>before_rows+10 && after_ordinal>before_ordinal+10,
        "actual History accumulator stops without GUI delivery");
    bool different=false;for(std::size_t band=0;band<50;++band){float first=0,newest=0;
        require(load_element(after_view,{0,after_view.actual[0]-after_filled,band},first)==Status::ok && first==oldest[band],"earlier History row overwritten before ring full");
        require(load_element(after_view,{0,after_view.actual[0]-1,band},newest)==Status::ok,"read latest History row");different|=newest!=first;}
    require(different,"History rows do not distinguish earlier/latest Mel values");
    std::printf("idle consumer snapshot before=%zu/%llu after=%zu/%llu dimensions=[%zu][%zu]\n",before_rows,
        (unsigned long long)before_ordinal,after_view.actual[0],(unsigned long long)after_ordinal,after_view.actual[0],after_view.actual[1]);snapshot={};
    require(wait_for([&]{return sink.ordinal>=after_ordinal+3;}),"compact History delivery stalled");
    print("growing consumer8.Input0",sink);std::printf("count7.Output1=%llu scalar12.Input0=%llu regularHistoryFrames=%zu errors=%zu older rows preserved\n",
        (unsigned long long)count_value,(unsigned long long)scalar_value,generic_history,generic_errors);
    require(!bad,"invalid compact History/count delivery before compatible Apply");
    // Force pending old-revision rows, so Apply's delivery fence is actually
    // exercised rather than depending on capture/event-loop timing.
    const auto delivered_before_apply=sink.ordinal;
    const auto prior_producer=sink.producer,prior_generation=sink.generation;
    QElapsedTimer backlog;backlog.start();
    do {
        QThread::msleep(10);snapshot={};
        require(engine.query_numeric(consumer,snapshot,&before_ordinal,&before_filled)==Status::ok,"pre-Apply cold History unavailable");
    }while(before_ordinal<delivered_before_apply+3 && backlog.elapsed()<1500);
    require(before_ordinal>=delivered_before_apply+2,"fixture did not queue old-revision History rows");
    const auto preserved_view=std::get<NumericView>(snapshot.frame.view().payload);
    std::vector<std::uint32_t> preserved_bits(before_filled*50);
    for(std::size_t row=0;row<before_filled;++row)for(std::size_t band=0;band<50;++band){
        float value=0;require(load_element(preserved_view,{0,preserved_view.actual[0]-before_filled+row,band},value)==Status::ok,"pre-Apply retained History value");
        preserved_bits[row*50+band]=std::bit_cast<std::uint32_t>(value);
    }
    const auto preserved_rows=before_filled;snapshot={};
    graph.nodes[8].enabled=false;apply();
    require(wait_for([&]{return sink.ordinal>=before_ordinal+6 && count_value>=before_ordinal+6 && scalar_value>=before_ordinal+6;}),"compatible Apply resets or stalls History/count");
    require(!bad,"invalid compact History delivery during compatible Apply");
    require(source.producer==prior_producer && sink.producer==prior_producer &&
        source.generation==prior_generation && sink.generation==prior_generation,"compatible Apply changes source incarnation");
    require(source.revision_boundaries==1 && sink.revision_boundaries==1 &&
        source.boundary_skipped_rows>=2 && sink.boundary_skipped_rows>=2,"old-revision row fence was not exercised");
    require(engine.query_numeric(consumer,snapshot,&after_ordinal,&after_filled)==Status::ok,"post-Apply cold History unavailable");
    const auto retained_view=std::get<NumericView>(snapshot.frame.view().payload);
    require(retained_view.actual[0]==400 && after_filled>preserved_rows && after_ordinal>=before_ordinal+6,"compatible Apply clears accumulated History");
    for(std::size_t row=0;row<preserved_rows;++row)for(std::size_t band=0;band<50;++band){
        float value=0;require(load_element(retained_view,{0,retained_view.actual[0]-after_filled+row,band},value)==Status::ok &&
            std::bit_cast<std::uint32_t>(value)==preserved_bits[row*50+band],"compatible Apply lost a queued or delivered DSP History row");
    }
    snapshot={};std::printf("compatible Apply retained all%zu rows; old queued presentation rows fenced source%zu/sink%zu; within-revision ordinals contiguous\n",
        preserved_rows,source.boundary_skipped_rows,sink.boundary_skipped_rows);
    const auto continuous_ordinal=after_ordinal;QThread::msleep(60);QCoreApplication::processEvents();
    require(engine.query_numeric(consumer,snapshot,&after_ordinal,&after_filled)==Status::ok && after_ordinal>continuous_ordinal,"core history paused after compatible Apply");
    require(!bad,"invalid continuous History/count delivery");
    std::printf("hot regularHistoryFrames=%zu errors=%zu\n",generic_history,generic_errors);
    require(generic_history==0,"hot regular History frames interleave compact row events on the same endpoint");
    std::puts("actual History provider failures=0");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"actual History provider FAIL: %s\n",e.what());return 1;}}
