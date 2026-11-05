#include "support.h"
#include <algorithm>
#include <chrono>
#include <cmath>

namespace rhythm_replay::detail {
using namespace signal_contracts;
using dataflow_runtime::NumericPortFrameView;
namespace {
double observed_time(const NumericPortFrameView& event) noexcept {
    const auto& clock=std::get<NumericView>(event.frame.payload).contract->clock;
    return double(event.frame.stamp.timestamp_ticks)*double(clock.tick_seconds.numerator)/double(clock.tick_seconds.denominator);
}
double latency_seconds(const FrameStamp& stamp) noexcept {
    return double(stamp.latency_seconds.numerator)/double(stamp.latency_seconds.denominator);
}
}
void Probe::configure(const char* label,domain::NodeId id,unsigned width,bool bytes_input,unsigned bands) {
    name=label;node=id;features=width;quantized=bytes_input;channels=bands;prepare();
}
void Probe::prepare() {
    dsp_kernels::OnsetConfig config;config.feature_count=features;config.channel_count=channels;
    if(channels)config.channels={dsp_kernels::OnsetBand{0,24},{24,8},{32,16},{48,48}};
    require(detector.prepare(config),"Cannot prepare replay onset probe");
    require(fusion.prepare(std::max(1U,channels)),"Cannot prepare replay onset fusion");
}
void Probe::account(unsigned i) noexcept {
    minimum=(frames || i)?std::min(minimum,double(floats[i])):double(floats[i]);maximum=std::max(maximum,double(floats[i]));
    zeros+=floats[i]==0;above_one+=floats[i]>1;upper_u8+=quantized && bytes[i]==255;
}
bool Probe::read(const NumericView& view) noexcept {
    if(view.actual[0]!=features || view.planes.size()!=1)return false;
    if(view.contract->dtype!=(quantized?DType::UInt8:DType::Float32))return false;
    for(unsigned i=0;i<features;++i)if(!read_element(view,i))return false;
    return true;
}
bool Probe::read_element(const NumericView& view,unsigned index) noexcept {
    if(quantized){
        if(load_element(view,{0,index,0},bytes[index])!=Status::ok)return false;
        floats[index]=float(bytes[index])/255.f;
    }else if(load_element(view,{0,index,0},floats[index])!=Status::ok)return false;
    if(!std::isfinite(floats[index]) || floats[index]<0)return false;
    account(index);return true;
}
bool Probe::process(double observed,double available) noexcept {
    const bool accepted=quantized?detector.process(std::span{bytes}.first(features),observed,available)
        :detector.process(std::span{floats}.first(features),observed,available);
    if(!accepted || !fusion.process(detector.output(),observed,available))return false;
    const auto dropped=fusion.dropped();dropped_peaks+=dropped;
    if(dropped){detector.reset();fusion.reset();}
    return true;
}
bool Observations::write_features(const Probe& probe,double observed) noexcept {
    if(probe.node==15 || probe.channels)return true;
    for(unsigned i=0;i<probe.features;++i)
        if(std::fprintf(features_.get(),"%s,%llu,%.17g,%u,%.9g\n",probe.name,static_cast<unsigned long long>(probe.frames),observed,i,double(probe.floats[i]))<0)return false;
    return true;
}
bool Observations::write_channels(Probe& probe,double observed,double available,double process_us,double latency) noexcept {
    unsigned channel=0;
    for(const auto& value:probe.detector.output()){
        if(std::fprintf(observations_.get(),"%s,%llu,%.17g,%.17g,%u,%.17g,%.17g,%.17g,%.17g,%u,%.6g,%.17g,%.17g\n",
            probe.name,static_cast<unsigned long long>(probe.frames),observed,available,channel,value.flux,value.contrast,value.threshold,value.mean_level,value.above_unit_values,process_us,latency,value.pending_t_obs.value_or(-1))<0)return false;
        if(value.peak){
            const auto& peak=*value.peak;++probe.peaks;
            if(std::fprintf(events_.get(),"%s,%llu,%u,%.17g,%.17g,%.17g,%.17g\n",probe.name,static_cast<unsigned long long>(probe.frames),channel,peak.t_obs,peak.t_ready,peak.strength,peak.positive_mass)<0)return false;
        }
        ++channel;
    }
    return true;
}
bool Observations::write_fused(Probe& probe) noexcept {
    for(const auto& value:probe.fusion.output()){
        const auto& peak=value.peak;++probe.fused_peaks;
        if(std::fprintf(fused_.get(),"%s,%llu,%u,%.17g,%.17g,%.17g,%.17g\n",probe.name,static_cast<unsigned long long>(probe.frames),unsigned(value.channels),peak.t_obs,peak.t_ready,peak.strength,peak.positive_mass)<0)return false;
    }
    return true;
}
bool Observations::write_probe(Probe& probe,const NumericPortFrameView& event) noexcept {
    const auto& view=std::get<NumericView>(event.frame.payload);
    if(!probe.read(view))return false;
    const double observed=observed_time(event),available=std::max(observed,available_);
    const auto start=std::chrono::steady_clock::now();
    if(!probe.process(observed,available))return false;
    const double microseconds=std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();
    ++probe.frames;
    return write_channels(probe,observed,available,microseconds,latency_seconds(event.frame.stamp)) && write_fused(probe) && write_features(probe,observed);
}
bool Observations::write_state(const NumericPortFrameView& event) noexcept {
    float value=0;
    if(event.frame.stamp.available && load_element(std::get<NumericView>(event.frame.payload),{0,0,0},value)!=Status::ok)return false;
    return std::fprintf(state_.get(),"%llu,%.17g,%u,%u,%.9g\n",static_cast<unsigned long long>(event.frame.stamp.sequence),observed_time(event),unsigned(event.identity.port),unsigned(event.frame.stamp.available),double(value))>0;
}
bool Observations::accept_status(const NumericPortFrameView& event) noexcept {
    if(event.status==Status::ok || event.status==Status::unavailable)return true;
    error_="Replay node rejected data";failed_node_=event.identity.node;failed_port_=event.identity.port;failed_status_=event.status;
    return false;
}
bool Observations::write_probes(const NumericPortFrameView& event) noexcept {
    for(auto& probe:probes_){
        if(event.identity.node!=probe.node || event.identity.port!=0)continue;
        if(event.status!=Status::ok || !write_probe(probe,event))return false;
    }
    return true;
}
void Observations::receive(const NumericPortFrameView& event) noexcept {
    if(error_ || event.direction!=domain::PortDirection::Output || !accept_status(event))return;
    if(event.identity.node==1102){if(!write_state(event))error_="Cannot record runtime state";return;}
    if(!event.frame.stamp.available)return;
    write_available(event);
}
void Observations::write_available(const NumericPortFrameView& event) noexcept {
    if(!reference_.receive(event,available_)){error_=reference_.failure();return;}
    if(!write_tempograms(event)){error_="Invalid tempogram frame or CSV write";return;}
    if(!write_probes(event))error_="Invalid onset probe frame or CSV write";
}
void Observations::receive(void* user,const NumericPortFrameView& event) noexcept {static_cast<Observations*>(user)->receive(event);}
void Observations::check() const {
    if(failed_node_>=0)throw std::runtime_error("Replay node "+std::to_string(failed_node_)+" port "+std::to_string(failed_port_)+" status "+std::to_string(unsigned(failed_status_)));
    require(error_==nullptr,error_?error_:"");
}
}
