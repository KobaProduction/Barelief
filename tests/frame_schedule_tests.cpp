#include <visualization/rendering/frame_schedule.h>
#include <cstdlib>
#include <iostream>
#include <utility>
using visualization::rendering::FrameSchedule;
void require(bool value,const char* message){if(!value){std::cerr<<message<<'\n';std::exit(1);}}
void outstanding_frame_and_burst(){
    FrameSchedule s;s.setRate(100);
    require(!s.submit(1),"idle created a frame");
    s.request();const auto first=s.submit(1);require(first!=0,"first frame missing");
    for(int i=0;i<10000;++i)s.request();
    require(!s.submit(100000001),"burst bypassed outstanding frame");
    require(!s.complete(first+1),"foreign completion accepted");
    require(s.complete(first),"completion rejected");
    const auto second=s.submit(100000001);require(second>first,"latest dirty state missing");
    require(s.complete(second),"second completion rejected");
    require(!s.submit(200000001),"idle repaint loop");
}
void visibility_and_stale_completion(){
    FrameSchedule s;s.setRate(100);s.request();const auto first=s.submit(1);
    require(s.complete(first),"first completion rejected");
    s.request();s.setVisible(false);require(!s.submit(300000001),"hidden submitted");
    s.setVisible(true);const auto third=s.submit(300000001);
    s.request();s.setVisible(false);s.setVisible(true);const auto fourth=s.submit(300000002);
    require(fourth>third && !s.complete(third),"stale completion released new frame");
    require(s.complete(fourth),"new completion rejected");
    s.request();require(s.delay(300000003)>0 && !s.submit(300000003),"rate limit bypassed");
    require(s.submit(310000002)!=0,"deadline frame missing");
}
void jitter_and_rate_change(){
    FrameSchedule phase;phase.setRate(100);phase.request();const auto token=phase.submit(100);
    phase.complete(token);phase.request();const auto late=phase.submit(11000100);
    require(late!=0,"ordinary jitter missing");phase.complete(late);phase.request();
    require(phase.delay(11000100)==9000000,"jitter shifted cadence phase");
    phase.setRate(200);require(phase.submit(11000100)!=0,"new rate retained old deadline");
}
void high_refresh_cadence(){
    for(const auto [rate,period]:{std::pair{360,2777778ULL},std::pair{1000,1000000ULL}}){
        FrameSchedule fast;fast.setRate(rate);fast.request();const auto token=fast.submit(1);
        require(token!=0 && fast.complete(token),"high-refresh first submission missing");
        fast.request();require(fast.delay(1)==period,"high-refresh cadence was silently capped");
        require(!fast.submit(period) && fast.submit(period+1)!=0,"high-refresh deadline mismatch");
    }
}
int main(){
    outstanding_frame_and_burst();
    visibility_and_stale_completion();
    jitter_and_rate_change();
    high_refresh_cadence();
    std::cout<<"frame_schedule_tests PASS\n";
}
