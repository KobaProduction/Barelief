#include <pipeline/numeric_engine.h>
#include <pipeline/test_signal.h>
#include <core_app/numeric_default_graph.h>
#include <desktop/numeric_project_document.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QThread>
#include <cstdio>
#include <cmath>
#include <array>
#include <stdexcept>
static void require(bool b,const char* text){if(!b)throw std::runtime_error(text);}
static bool wait(std::function<bool()> f){QElapsedTimer t;t.start();while(!f()&&t.elapsed()<2000){QCoreApplication::processEvents();QThread::msleep(1);}return f();}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    for(const std::uint32_t rate:{8000u,48000u,192000u}) {
        const std::uint64_t samples=std::uint64_t(rate)*86400*7;
        std::int64_t timestamp=0,next=0;
        require(pipeline::test_signal_sample_time_ns(samples,rate,17,timestamp) && timestamp==604800000000017LL,"seven-day sample clock overflow");
        require(pipeline::test_signal_sample_time_ns(samples+rate,rate,17,next) && next-timestamp==1000000000LL,"long-run sample clock is not monotonic");
    }
    std::int64_t refused=71;require(!pipeline::test_signal_sample_time_ns(UINT64_MAX,48000,0,refused)&&refused==71,"exhausted clock published partial timestamp");
    pipeline::TestSignalSettings settings;QString error;
    for(const auto& invalid:{QJsonObject{{"tones",4}},QJsonObject{{"tones",8.5}},QJsonObject{{"amplitude",1.1}},QJsonObject{{"maximum_hz",30000}},QJsonObject{{"period_seconds",0}}})require(!pipeline::TestSignalSettings::parse(invalid,settings,error),"invalid source settings accepted");
    auto graph=numeric_default_graph();graph.nodes[0].kind="TestSignal";
    auto& params=std::get<domain::EndpointProcessingParameters>(graph.nodes[0].parameters);params.values.push_back({"amplitude",signal_contracts::NumericLiteral{.125}});
    pipeline::NumericEngine engine([](auto){return pipeline::make_test_signal_source();});bool accepted=false,rejected=false;
    engine.set_reply_handler([&](const auto& reply){accepted=reply.accepted;rejected=!reply.accepted;});
    const pipeline::EndpointIdentity pcm{"main",9,domain::PortDirection::Input,0},history{"main",7,domain::PortDirection::Output,0};
    require(engine.subscribe_numeric_frames({pcm}),"subscribe PCM");std::size_t received=0;bool bad=false,nonzero=false,stereo=false;
    engine.set_numeric_frame_handler([&](auto owned){if(!owned.frame)return;const auto frame=owned.frame.view();if(!frame.stamp.available)return;const auto* numeric=std::get_if<signal_contracts::NumericView>(&frame.payload);if(!numeric)return;
        bad|=numeric->actual[0]!=256 || numeric->planes.size()!=2 || numeric->contract->clock.sample_rate!=48000;
        for(std::size_t i=0;i<numeric->actual[0];++i){float left=0,right=0;
            if(signal_contracts::load_element(*numeric,{0,i,0},left)!=signal_contracts::Status::ok || signal_contracts::load_element(*numeric,{1,i,0},right)!=signal_contracts::Status::ok){bad=true;continue;}
            const auto l=left,r=right;bad|=!std::isfinite(l)||!std::isfinite(r)||std::abs(l)>.12501||std::abs(r)>.12501;nonzero|=std::abs(l)>1e-6;stereo|=std::abs(l-r)>1e-6;
        }++received;});
    auto apply=[&]{QByteArray bytes;require(bool(desktop::encode_numeric_graph(graph,bytes)),"encode canonical graph");pipeline::EngineCommand command;command.kind=pipeline::EngineCommand::ApplyGraph;command.graph=QJsonDocument::fromJson(bytes).object();accepted=rejected=false;engine.submit(command);require(wait([&]{return accepted||rejected;})&&accepted,"same application Apply rejected");};
    apply();require(wait([&]{return received>=10;}),"silent source delivery");require(!bad&&nonzero&&stereo,"actual configured stereo signal/clock/amplitude");
    // Producer and executor advance while the GUI event loop is not serviced.
    pipeline::DeliveredNumericFrame before,after;std::uint64_t filled=0,after_filled=0;require(engine.query_numeric(history,before,nullptr,&filled)==signal_contracts::Status::ok,"cold history before");require(std::get<signal_contracts::NumericView>(before.frame.view().payload).actual[0]==400,"cold History capacity");
    QThread::msleep(400);require(engine.query_numeric(history,after,nullptr,&after_filled)==signal_contracts::Status::ok,"cold history after");require(std::get<signal_contracts::NumericView>(after.frame.view().payload).actual[0]==400 && after_filled>filled+10,"processing depends on GUI event delivery");
    std::uint64_t old_ordinal=0;require(engine.query_numeric(history,before,&old_ordinal,&filled)==signal_contracts::Status::ok,"history before live edit");const auto old_stamp=before.frame.view().stamp;std::array<float,50> oldest{};const auto old_view=std::get<signal_contracts::NumericView>(before.frame.view().payload);for(std::size_t band=0;band<50;++band)require(signal_contracts::load_element(old_view,{0,old_view.actual[0]-filled,band},oldest[band])==signal_contracts::Status::ok,"read live edit old row");before={};std::size_t zero_frames=0;std::int64_t last_pcm_timestamp=0;std::uint64_t last_pcm_sequence=0;
    params.values[0].value=signal_contracts::NumericLiteral{0.0};engine.set_numeric_frame_handler([&](auto owned){if(!owned.frame)return;const auto f=owned.frame.view();if(!f.stamp.available)return;require(f.stamp.timestamp_ticks>last_pcm_timestamp && f.stamp.source_sequence>last_pcm_sequence,"source sample clock did not advance");last_pcm_timestamp=f.stamp.timestamp_ticks;last_pcm_sequence=f.stamp.source_sequence;const auto* n=std::get_if<signal_contracts::NumericView>(&f.payload);if(!n)return;float v=0;require(signal_contracts::load_element(*n,{0,100,0},v)==signal_contracts::Status::ok,"zero PCM element");if(zero_frames++>=2)require(v==0,"source configuration not applied");++received;});const auto previous=received;apply();require(wait([&]{return received>previous+3;}),"reconfigured silent source");
    QThread::msleep(120);std::uint64_t edited_ordinal=0;require(engine.query_numeric(history,after,&edited_ordinal,&after_filled)==signal_contracts::Status::ok,"history after live edit");const auto edited_stamp=after.frame.view().stamp;require(edited_ordinal>old_ordinal && edited_stamp.producer_epoch==old_stamp.producer_epoch,"live parameter edit restarted source clock/history");const auto edited_view=std::get<signal_contracts::NumericView>(after.frame.view().payload);for(std::size_t band=0;band<50;++band){float value=0;require(signal_contracts::load_element(edited_view,{0,edited_view.actual[0]-after_filled,band},value)==signal_contracts::Status::ok && value==oldest[band],"live parameter edit discarded historical rows");}after={};
    graph.nodes[0].enabled=false;const auto muted_before=received;apply();
    require(wait([&]{return received>muted_before+3;}),"disabled source clock stopped");
    require(engine.snapshot().statuses.value("capture").state==pipeline::State::Running,"disabled source stopped physical producer");
    return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
