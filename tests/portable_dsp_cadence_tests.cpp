#include "canonical_runtime_test_fixture.h"
using namespace canonical_test;
struct Windows{std::size_t count=0;std::int64_t end=0;std::uint64_t sequence=0;};
static void window(void* user,const NumericPortFrameView& f){if(f.identity.node!=2||f.status!=Status::ok)return;auto& w=*static_cast<Windows*>(user);++w.count;check(f.frame.stamp.sequence>w.sequence,"window sequence");w.sequence=f.frame.stamp.sequence;w.end=f.frame.stamp.timestamp_ticks;check(w.count==1||!f.frame.stamp.discontinuity,"unexpected cadence discontinuity");}
int main(){for(std::size_t rate:{44100,48000,44101}){
 auto g=numeric_test_graph();std::erase_if(g.nodes,[](const auto& n){return n.id>2;});g.edges={{1,2,0,0}};geometry(g,rate,512,317);
 GraphExecutor e(profile());application::PreparedExecution pending;prepare(e,g,pending);check(e.apply(std::move(pending))==Status::ok,"apply cadence");Feed feed(g);Windows w;
 while(feed.sent<rate)feed.process(e,std::min<std::size_t>(317,rate-feed.sent),{window,nullptr,nullptr,&w});
 check(w.count==60&&w.end==std::int64_t(rate),"one second must produce sixty sample-clock windows");std::printf("cadence rate%zu windows%zu end%lld PASS\n",rate,w.count,(long long)w.end);
}}
