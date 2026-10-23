#pragma once
#include <dsp_kernels/rhythm.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <tuple>

namespace dsp_kernels::rhythm::detail {
template<class T,std::size_t N> struct Ring {
    std::array<T,N> values{};
    std::size_t start=0, count=0;
    const T& operator[](std::size_t i) const noexcept {return values[(start+i)%N];}
    T& operator[](std::size_t i) noexcept {return values[(start+i)%N];}
    void clear() noexcept {start=0;count=0;}
    void pop() noexcept {if(count){start=(start+1)%N;--count;}}
    void push(const T& value) noexcept {
        if(count==N)pop();
        values[(start+count)%N]=value;++count;
    }
    const T& back() const noexcept {return (*this)[count-1];}
};
template<class T,std::size_t N> struct List {
    std::array<T,N> values{};
    std::size_t count=0;
    void clear() noexcept {count=0;}
    void push(const T& value) noexcept {if(count<N)values[count++]=value;}
    auto span() noexcept {return std::span<T>{values.data(),count};}
    auto span() const noexcept {return std::span<const T>{values.data(),count};}
};
struct Group {
    std::uint32_t id=0;
    Ring<double,96> times;
    Ring<double,24> strong;
    std::optional<double> stable_period;
};
struct Acf {double bpm=0, score=0;};
struct Family {double period_s=0,bpm=0,score=0,accent_score=0;bool accented=false;};
struct Phase {std::optional<double> value;double fit=0;};
double rounded(double value,unsigned digits) noexcept;
double modulo(double value,double period) noexcept;
bool strong(Kind kind) noexcept;
bool octave(double a,double b) noexcept;
bool equivalent(double a,double b,unsigned fps) noexcept;
Phase phase_fit(std::span<const double> times,double period) noexcept;
Phase phase_fit(const Ring<double,96>& times,double period) noexcept;
double ioi_support(const Ring<double,96>& times,double period) noexcept;
Estimate estimate(Status status,std::optional<double> score,std::optional<double> observed,
                  const Frame& frame,Reason reason=Reason::None) noexcept;
bool valid_config(const Config& config,Mode mode) noexcept;
bool valid_frame(const Frame& frame) noexcept;
void clear_output(Output& output) noexcept;
struct Engine {
    Config cfg;
    Mode mode=Mode::Gated;
    Output out;
    bool initialized=false;
    std::uint64_t epoch=0,profile=0,seq=0,sample_index=0,event_sequence=0;
    double sample_rate=0,t_obs=0;
    std::array<std::uint8_t,96> features{};
    unsigned width=0,history_count=0,overflow=0,recovery=0;
    Ring<double,511> flux;
    Ring<double,96> times;
    Ring<Accent,256> typed,seen;
    List<Group,8> groups;
    List<Acf,512> acf;
    std::optional<double> last_onset,last_heavy_obs,stable_period,tracking_start;
    std::optional<double> primary_conflict,subdivision_conflict;
    std::optional<std::uint64_t> last_heavy_seq;
    std::optional<std::uint32_t> anchor;
    std::optional<std::pair<double,double>> last_determined;
    std::optional<double> spectral_last,spectral_stable;
    Ring<double,23> spectral_initial,spectral_pending;
    Change change,spectral_change;
    // At most 8*95*3 IOI proposals, 512 ACF proposals and their two shadows.
    List<double,8376> periods;
    // With max/min BPM <= 10, the bounded 1.5% family clustering needs < 312.
    List<Family,512> families;
    List<double,256> salient,weak;
    std::array<double,96> contributions{};
    std::array<double,512> centered{};

    void reset(bool preserve_sequence=false) noexcept;
    Reset accept_source(const Frame& frame) noexcept;
    void observe(const Frame& frame) noexcept;
    void ingest(const Frame& frame,Accent item) noexcept;
    void expire(double now) noexcept;
    Group* group(std::uint32_t id,bool create=false) noexcept;
    void strong_change(const Event& event) noexcept;
    void confirm_change(Group& group,double period,double start,double now,double spread) noexcept;
    void spectral_rate(const Event& event) noexcept;
    void subdivision(const Event& event) noexcept;
    void track_changes() noexcept;
    bool recent_intervals(const Ring<double,24>& values,double& period,double& spread) const noexcept;
    void trim_episode(double start) noexcept;
    unsigned possible_ops() const noexcept;
    void refine(const Frame& frame,bool ambiguous) noexcept;
    void calculate_acf(unsigned length) noexcept;
    void propose(double period) noexcept;
    void proposals(bool use_acf) noexcept;
    Hierarchy hierarchy(double period,double now) noexcept;
    void hierarchy_times(double now,std::uint8_t& strong_groups,std::uint8_t& weak_groups) noexcept;
    Candidate candidate(double period,double now,bool use_acf) noexcept;
    void candidate_phases(Candidate& value) noexcept;
    void candidate_support(Candidate& value,double now,bool use_acf) noexcept;
    void cluster(double now,bool use_acf) noexcept;
    void rank(double now,bool use_acf) noexcept;
    void keep_candidate(const Candidate& value) noexcept;
    Octave octave_decision() const noexcept;
    std::optional<std::size_t> relock(double now) const noexcept;
    void decide(const Frame& frame,bool use_acf) noexcept;
    void publish_tempo(const Frame& frame,bool use_acf) noexcept;
    void publish_grid(const Frame& frame) noexcept;
    void publish_dynamics(const Frame& frame) noexcept;
    void publish_observations(const Frame& frame) noexcept;
    void process(const Frame& frame) noexcept;
};
}
