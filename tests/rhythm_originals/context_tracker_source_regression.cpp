#include <rhythm_reference_ports/context_beat_sequence.h>
#include "../aubio_context_test_inputs.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
namespace {
void check(bool value,const char* message) {
    if(!value){std::fprintf(stderr,"context source regression: %s\n",message);std::exit(1);}
}
// Actual aubio ad5cf975 beattracking_do(512,512,44100), one fresh ODF
// window: zero -> {0,43,86}; denorm_min -> {1,44,87}; period 22016.
// Both have repeated maxima and confidence zero. No inferred beat evidence.
void tied_source_window(float level,float first) {
    rhythm_reference_ports::PreparedContextBeatTracker tracker;
    check(tracker.prepare({512,44100.f/512.f}),"prepare source geometry");
    std::array<float,512> input{};input.fill(level);
    aubio_test::ContextInputs frontend;check(frontend.prepare() && frontend.analyze(input),"explicit weighted history and ACF");
    std::array<float,129> offsets{};std::size_t count=0;
    check(tracker.process(frontend.reversed,frontend.acf,offsets,count),"process tied source window");
    check(count==3,"source produces three predicted offsets");
    for(std::size_t i=0;i<count;++i)check(offsets[i]==first+43.f*float(i),"last equal maximum preserves source phase alignment");
    check(tracker.period_frames()==43 && tracker.confidence()==0,"raw estimate remains visible without confidence");
}
void check_silent_publication(rhythm_reference_ports::PreparedContextBeatSequence& sequence,
                              const rhythm_reference_ports::ContextBeatFrame& frame,std::size_t i) {
    check(frame.silent && frame.source_tactus==0,"silence does not publish a confirmed beat");
    if(i<127)check(!frame.scheduled && sequence.last_beat_sample()==0,"no schedule before the first context update");
    if(i==127)check(frame.scheduled && frame.beat_sample==65024,"source silent schedule updates the raw last-beat sample");
    check(sequence.last_tatum_sample()==(i==127?65024:0),"getter precedes explicit tatum polling as in the reference");
    check(sequence.poll_tatum()==0,"matching source poll order has no tatum event");
}
void silent_scheduled_timestamps() {
    rhythm_reference_ports::PreparedContextBeatSequence sequence;
    check(sequence.prepare(),"prepare source default context sequence");
    aubio_test::ContextInputs frontend;check(frontend.prepare() && frontend.push(0),"explicit silent context inputs");
    rhythm_reference_ports::ContextBeatFrame frame;
    for(std::size_t i=0;i<128;++i){
        check(sequence.process(frontend.reversed,frontend.acf,true,frame),"silent ODF frame");
        check_silent_publication(sequence,frame,i);
    }
    check(sequence.period_samples()==22016 && sequence.tracker().confidence()==0,"silent source period has no confidence");
    check(std::abs(sequence.tracker().bpm()-120.1853256225586f)<2e-5f,"source silence BPM estimate");
}
}
int main() {
    tied_source_window(0,0);tied_source_window(std::numeric_limits<float>::denorm_min(),1);
    silent_scheduled_timestamps();std::puts("aubio last-tie phase/silent scheduled timestamps PASS");
}
