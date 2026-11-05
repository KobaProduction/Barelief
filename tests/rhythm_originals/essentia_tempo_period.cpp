#include "essentia_tempo_export.h"
#include <rhythm_reference_ports/period_beat_tracker.h>
namespace essentia_tempo_export {
void period(const Case& c) {
    rhythm_reference_ports::PreparedPeriodBeatTracker tracker;
    rhythm_reference_ports::PeriodBeatTrackingConfig config;
    config.frame_rate=float(c.data.rate)/float(c.data.hop);config.noise={nullptr,noise};
    native_export::require(tracker.prepare(c.data.pcm.size(),config),"period tracker prepare");
    std::vector<std::uint8_t> mask(tracker.output_samples(c.data.pcm.size()));
    std::vector<float> ticks(mask.size());std::size_t count=0;
    native_export::require(tracker.process(c.data.pcm,mask) && tracker.beat_times(ticks,count),"period tracker process");
    native_export::Output out(c.data.prefix);
    out.row<float>("ticks.f32",std::span(ticks).first(count));out.row<float>("periods.f32",tracker.periods_seconds());
    out.row<float>("ends.f32",tracker.block_ends_seconds());out.row<float>("normalized.f32",tracker.normalized_curve());
    out.row<std::uint8_t>("mask.u8",mask);out.row<std::uint32_t>("states.u32",tracker.state_path());
}
}
