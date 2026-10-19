#include <pipeline/numeric_engine.h>
#include <pipeline/test_signal.h>
#include <core_app/numeric_default_graph.h>
#include <desktop/numeric_project_document.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QThread>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <utility>
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
namespace {
struct ColdHistory {
    DeliveredNumericFrame frame;
    std::uint64_t ordinal=0,filled=0;
    NumericView view() const {return std::get<NumericView>(frame.frame.view().payload);}
};
struct RetainedHistory {
    std::uint64_t ordinal=0,rows=0,producer=0,generation=0;
    std::vector<std::uint32_t> bits;
};
bool valid_row(const DeliveredNumericHistoryRow& row,const NumericView& numeric) {
    return numeric.contract->semantic==Semantic::MelEnergy && numeric.actual==std::array<std::size_t,2>{50,1}
        && numeric.planes.size()==1 && row.capacity==400
        && row.filled_count==std::min(row.accepted_row_ordinal,std::uint64_t{400});
}
bool revision_boundary(const DeliveredNumericHistoryRow& row,const FrameStamp& stamp,Signature& signature) {
    if(row.compact.delivery.epoch!=signature.delivery_epoch || row.compact.delivery.graph_revision<=signature.delivery_revision
        || row.accepted_row_ordinal<=signature.ordinal || stamp.sequence<=signature.sequence || stamp.discontinuity
        || stamp.sequence-signature.sequence!=row.accepted_row_ordinal-signature.ordinal)return false;
    ++signature.revision_boundaries;
    signature.boundary_skipped_rows+=row.accepted_row_ordinal-signature.ordinal-1;
    return true;
}
bool contiguous_row(const DeliveredNumericHistoryRow& row,const FrameStamp& stamp,Signature& signature) {
    if(!signature.ordinal || stamp.producer_epoch!=signature.producer || stamp.generation!=signature.generation)return true;
    if(row.compact.delivery.epoch!=signature.delivery_epoch || row.compact.delivery.graph_revision!=signature.delivery_revision)
        return revision_boundary(row,stamp,signature);
    if(row.accepted_row_ordinal==signature.ordinal+1 && stamp.sequence==signature.sequence+1 && !stamp.discontinuity)return true;
    std::fprintf(stderr,"History ordinal gap within revision node%d old%llu new%llu revision%llu\n",row.compact.context->endpoint.node,
        (unsigned long long)signature.ordinal,(unsigned long long)row.accepted_row_ordinal,(unsigned long long)signature.delivery_revision);
    return false;
}
bool record_values(const NumericView& numeric,Signature& signature) {
    std::array<float,50> values{};bool valid=true;
    for(std::size_t band=0;band<values.size();++band){
        if(load_element(numeric,{0,band,0},values[band])!=Status::ok || !std::isfinite(values[band]))valid=false;
        signature.nonzero|=values[band]>0;
    }
    if(signature.rows && values!=signature.latest)signature.changed=true;
    signature.latest=values;
    return valid;
}
void record_identity(const DeliveredNumericHistoryRow& row,const FrameStamp& stamp,Signature& signature) {
    signature.ordinal=row.accepted_row_ordinal;signature.filled=row.filled_count;
    signature.discontinuities+=stamp.discontinuity;
    signature.producer=stamp.producer_epoch;signature.generation=stamp.generation;signature.sequence=stamp.sequence;
    ++signature.rows;
    signature.delivery_revision=row.compact.delivery.graph_revision;signature.delivery_epoch=row.compact.delivery.epoch;
}
std::array<float,50> history_row(const NumericView& view,std::size_t index) {
    std::array<float,50> result{};
    for(std::size_t band=0;band<result.size();++band)
        require(load_element(view,{0,index,band},result[band])==Status::ok,"read cold History row");
    return result;
}
void capture_retained_row(const NumericView& view,std::size_t filled,std::size_t row,std::vector<std::uint32_t>& bits) {
    const auto values=history_row(view,view.actual[0]-filled+row);
    for(std::size_t band=0;band<values.size();++band)bits[row*50+band]=std::bit_cast<std::uint32_t>(values[band]);
}
void verify_retained_row(const NumericView& view,std::size_t filled,std::size_t row,const std::vector<std::uint32_t>& bits) {
    const auto values=history_row(view,view.actual[0]-filled+row);
    for(std::size_t band=0;band<values.size();++band)
        require(std::bit_cast<std::uint32_t>(values[band])==bits[row*50+band],"compatible Apply lost a queued or delivered DSP History row");
}
class HistoryProviderScenario {
    domain::NumericProcessingGraph graph=numeric_test_graph();
    const EndpointIdentity producer{"main",7,domain::PortDirection::Output,0};
    const EndpointIdentity consumer{"main",8,domain::PortDirection::Input,0};
    const EndpointIdentity count{"main",7,domain::PortDirection::Output,1};
    const EndpointIdentity scalar{"main",12,domain::PortDirection::Input,0};
    Signature source,sink;
    std::uint64_t count_value=0,scalar_value=0;
    std::size_t generic_history=0,generic_errors=0;
    bool bad=false,accepted=false,rejected=false;
    // Destroy the engine and its callbacks before the state they reference.
    NumericEngine engine{[](auto){return make_test_signal_source();}};
    Signature* signature(const EndpointIdentity& endpoint) {
        return endpoint==producer?&source:endpoint==consumer?&sink:nullptr;
    }
    void receive_history(DeliveredNumericHistoryRow row) {
        if(!row.compact.context || row.event!=NumericHistoryEvent::Row)return;
        auto* current=signature(row.compact.context->endpoint);if(!current)return;
        if(row.compact.status!=Status::ok || !row.compact.frame){bad=true;return;}
        const auto frame=row.compact.frame.view();const auto* numeric=std::get_if<NumericView>(&frame.payload);
        if(!numeric || !valid_row(row,*numeric)){bad=true;return;}
        if(!contiguous_row(row,frame.stamp,*current))bad=true;
        if(!record_values(*numeric,*current))bad=true;
        record_identity(row,frame.stamp,*current);
    }
    void receive_frame(DeliveredNumericFrame delivered) {
        if(!delivered.context)return;
        const auto& endpoint=delivered.context->endpoint;
        if(endpoint==producer || endpoint==consumer){++generic_history;generic_errors+=delivered.status!=Status::ok;return;}
        if(!delivered.frame || delivered.status!=Status::ok || (endpoint!=count && endpoint!=scalar))return;
        const auto frame=delivered.frame.view();const auto* numeric=std::get_if<NumericView>(&frame.payload);
        std::uint64_t value=0;
        if(!numeric || numeric->contract->dtype!=DType::UInt64 || numeric->contract->shape.rank!=Rank::Scalar
            || load_element(*numeric,{0,0,0},value)!=Status::ok || value>400){bad=true;return;}
        (endpoint==count?count_value:scalar_value)=value;
    }
    void bind_delivery() {
        require(engine.subscribe_numeric_frames({producer,consumer,count,scalar}),"subscribe actual History and count endpoints");
        engine.set_numeric_history_handler([this](DeliveredNumericHistoryRow row){receive_history(std::move(row));});
        engine.set_numeric_frame_handler([this](DeliveredNumericFrame frame){receive_frame(std::move(frame));});
        engine.set_reply_handler([this](const EngineReply& reply){accepted=reply.accepted;rejected=!reply.accepted;});
    }
    void submit(EngineCommand command) {
        accepted=rejected=false;engine.submit(std::move(command));
        require(wait_for([this]{return accepted||rejected;})&&accepted,"engine command rejected");
    }
    void apply() {
        QByteArray bytes;require(bool(desktop::encode_numeric_graph(graph,bytes)),"encode actual default graph");
        EngineCommand command;command.kind=EngineCommand::ApplyGraph;command.graph=QJsonDocument::fromJson(bytes).object();
        submit(std::move(command));
    }
    ColdHistory query(const char* message) {
        ColdHistory result;
        require(engine.query_numeric(consumer,result.frame,&result.ordinal,&result.filled)==Status::ok,message);
        return result;
    }
    void start() {
        bind_delivery();apply();
        require(wait_for([this]{return sink.filled>=20 && count_value>=20 && scalar_value>=20;}),"History/count absent at actual provider consumer");
        require(!bad && source.nonzero && sink.nonzero && sink.changed,"invalid or constant actual compact History delivery");
        print("producer7.Output0",source);print("consumer8.Input0",sink);
    }
    void idle_snapshot(const ColdHistory& after,const ColdHistory& before,const std::array<float,50>& oldest) {
        const auto view=after.view();
        require(view.contract->semantic==Semantic::History && view.actual[1]==50 && view.actual[0]==400
            && after.filled>before.filled+10 && after.ordinal>before.ordinal+10,"actual History accumulator stops without GUI delivery");
        const auto first=history_row(view,view.actual[0]-after.filled);
        const auto newest=history_row(view,view.actual[0]-1);
        require(first==oldest,"earlier History row overwritten before ring full");
        require(newest!=first,"History rows do not distinguish earlier/latest Mel values");
        std::printf("idle consumer snapshot before=%llu/%llu after=%zu/%llu dimensions=[%zu][%zu]\n",
            (unsigned long long)before.filled,(unsigned long long)before.ordinal,view.actual[0],
            (unsigned long long)after.ordinal,view.actual[0],view.actual[1]);
    }
    std::uint64_t grow_without_delivery() {
        auto before=query("consumer History snapshot unavailable");
        require(before.view().actual[0]==400,"cold snapshot must expose fixed capacity");
        before.frame={};
        auto initial=query("snapshot first row");
        const auto oldest=history_row(initial.view(),initial.view().actual[0]-initial.filled);
        const auto initial_ordinal=initial.ordinal;initial.frame={};
        before.ordinal=initial_ordinal;
        QThread::msleep(300);
        const auto after=query("consumer History snapshot after idle unavailable");
        idle_snapshot(after,before,oldest);
        return after.ordinal;
    }
    void resume_delivery(std::uint64_t ordinal) {
        require(wait_for([this,ordinal]{return sink.ordinal>=ordinal+3;}),"compact History delivery stalled");
        print("growing consumer8.Input0",sink);
        std::printf("count7.Output1=%llu scalar12.Input0=%llu regularHistoryFrames=%zu errors=%zu older rows preserved\n",
            (unsigned long long)count_value,(unsigned long long)scalar_value,generic_history,generic_errors);
        require(!bad,"invalid compact History/count delivery before compatible Apply");
    }
    ColdHistory queued_snapshot(std::uint64_t delivered) {
        QElapsedTimer backlog;backlog.start();ColdHistory snapshot;
        do {
            QThread::msleep(10);snapshot={};snapshot=query("pre-Apply cold History unavailable");
        }while(snapshot.ordinal<delivered+3 && backlog.elapsed()<1500);
        require(snapshot.ordinal>=delivered+2,"fixture did not queue old-revision History rows");
        return snapshot;
    }
    RetainedHistory capture_before_apply() {
        // Retire actual queued presentation rows while retaining the DSP rows.
        RetainedHistory result;result.producer=sink.producer;result.generation=sink.generation;
        const auto snapshot=queued_snapshot(sink.ordinal);
        result.ordinal=snapshot.ordinal;result.rows=snapshot.filled;result.bits.resize(result.rows*50);
        const auto view=snapshot.view();
        for(std::size_t row=0;row<result.rows;++row)capture_retained_row(view,result.rows,row,result.bits);
        return result;
    }
    void verify_delivery_boundary(const RetainedHistory& before) {
        require(!bad,"invalid compact History delivery during compatible Apply");
        require(source.producer==before.producer && sink.producer==before.producer
            && source.generation==before.generation && sink.generation==before.generation,"compatible Apply changes source incarnation");
        require(source.revision_boundaries==1 && sink.revision_boundaries==1
            && source.boundary_skipped_rows>=2 && sink.boundary_skipped_rows>=2,"old-revision row fence was not exercised");
    }
    std::uint64_t verify_retained(const RetainedHistory& before) {
        const auto after=query("post-Apply cold History unavailable");const auto view=after.view();
        require(view.actual[0]==400 && after.filled>before.rows && after.ordinal>=before.ordinal+6,"compatible Apply clears accumulated History");
        for(std::size_t row=0;row<before.rows;++row)verify_retained_row(view,after.filled,row,before.bits);
        std::printf("compatible Apply retained all%llu rows; old queued presentation rows fenced source%zu/sink%zu; within-revision ordinals contiguous\n",
            (unsigned long long)before.rows,source.boundary_skipped_rows,sink.boundary_skipped_rows);
        return after.ordinal;
    }
    std::uint64_t compatible_apply() {
        const auto before=capture_before_apply();graph.nodes[8].enabled=false;apply();
        require(wait_for([this,&before]{return sink.ordinal>=before.ordinal+6 && count_value>=before.ordinal+6
            && scalar_value>=before.ordinal+6;}),"compatible Apply resets or stalls History/count");
        verify_delivery_boundary(before);
        return verify_retained(before);
    }
    void continuous_delivery(std::uint64_t ordinal) {
        QThread::msleep(60);QCoreApplication::processEvents();
        const auto after=query("continuous History unavailable");
        require(after.ordinal>ordinal,"core history paused after compatible Apply");
        require(!bad,"invalid continuous History/count delivery");
        std::printf("hot regularHistoryFrames=%zu errors=%zu\n",generic_history,generic_errors);
        require(generic_history==0,"hot regular History frames interleave compact row events on the same endpoint");
    }
public:
    void run() {start();resume_delivery(grow_without_delivery());continuous_delivery(compatible_apply());}
};
}
int main(int argc,char** argv) {
    QCoreApplication application(argc,argv);
    try {HistoryProviderScenario scenario;scenario.run();std::puts("actual History provider failures=0");return 0;}
    catch(const std::exception& error){std::fprintf(stderr,"actual History provider FAIL: %s\n",error.what());return 1;}
}
