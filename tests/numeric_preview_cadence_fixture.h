#pragma once
#include "numeric_transport_fixture.h"
#include <control/numeric_desktop_host.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <QTimer>
namespace cadence_fixture {
using namespace transport_fixture;
struct CadenceObservation {
 std::size_t count=0,gaps=0;std::uint64_t previous=0;std::array<double,1024> gap_ms{};
 static std::uint64_t now(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
 void tick(){const auto time=now();if(previous&&gaps<gap_ms.size())gap_ms[gaps++]=double(time-previous)/1e6;previous=time;++count;}
 double p99_gap(){if(!gaps)return 0;std::sort(gap_ms.begin(),gap_ms.begin()+gaps);return gap_ms[(gaps*99+99)/100-1];}
};
struct CadenceScenario {
 NumericDesktopHost host;domain::NumericProcessingGraph graph=transport_fixture::graph();
 std::atomic_size_t calls{0},supplies{0},taps{0};std::atomic_bool stop{false},bad_feed{false};
 CadenceObservation engine_clock,baseline_clock;bool recording=false;QTimer* baseline=nullptr;
 std::uint64_t sample_start=0,sample_end=0,tap=0;int slow_interval=0,fast_interval=0;
 std::uint64_t epoch=0;std::size_t slow=0,slow_taps=0,slow_supplies=0;std::thread producer;
 void prepare(){
  {Replies replies(host);replies.accepted(apply(graph));}
  require(host.subscribe_numeric_frames({sink(graph)}),"cadence subscription");settle();
  tap=host.local_engine()->subscribe_numeric_tap({sink(graph)},[&](const auto&){++taps;},{});
  require(tap!=0,"cadence independent tap subscription");
  host.local_engine()->set_numeric_frame_handler([&](const auto& f){if(f.frame&&f.frame.view().stamp.available){++calls;if(recording)engine_clock.tick();}});
  host.set_numeric_preview_cadence_hz(20);
  QMetaObject::invokeMethod(host.local_engine(),[&]{slow_interval=host.local_engine()->numeric_preview_interval_ms();},Qt::BlockingQueuedConnection);
  epoch=host.snapshot().epoch;
 }
 void supply(const NumericContract& c,const PlaneLayout& plane,FrameStamp& stamp,std::uint64_t sequence){
  stamp.sequence=stamp.source_sequence=sequence;
  const auto status=host.local_engine()->feed_numeric(0,{stamp,NumericView{&c,std::as_bytes(std::span(&sequence,1)),std::span(&plane,1),{1,1}}});
  if(status!=Status::ok)bad_feed=true;
  ++supplies;
 }
 void produce(){
  auto c=contract();PlaneLayout plane{0,{8,0}};FrameStamp stamp;stamp.producer_epoch=epoch;stamp.generation=1;
  std::uint64_t sequence=0;auto deadline=std::chrono::steady_clock::now();
  while(!stop.load()){
   supply(c,plane,stamp,++sequence);deadline+=std::chrono::milliseconds(2);
   while(!stop.load()&&std::chrono::steady_clock::now()<deadline)std::this_thread::yield();
  }
 }
 void start(){producer=std::thread([&]{produce();});settle(500);slow=calls.load();slow_taps=taps.load();slow_supplies=supplies.load();}
 void start_baseline(){
  fast_interval=host.local_engine()->numeric_preview_interval_ms();
  baseline=new QTimer(host.local_engine());baseline->setTimerType(Qt::PreciseTimer);baseline->setInterval(8);
  QObject::connect(baseline,&QTimer::timeout,host.local_engine(),[&]{if(recording)baseline_clock.tick();});
  recording=true;sample_start=CadenceObservation::now();baseline->start();
 }
 void fast(){host.set_numeric_preview_cadence_hz(120);QMetaObject::invokeMethod(host.local_engine(),[&]{start_baseline();},Qt::BlockingQueuedConnection);}
 void finish_worker(){sample_end=CadenceObservation::now();recording=false;delete baseline;host.local_engine()->unsubscribe_numeric_tap(tap);host.local_engine()->set_numeric_frame_handler({});}
 void finish(bool require_native_target){
  settle(require_native_target?2000:1000);stop=true;producer.join();
  require(QMetaObject::invokeMethod(host.local_engine(),[&]{finish_worker();},Qt::BlockingQueuedConnection),"cadence callback teardown worker barrier");
 }
 void report(bool require_native_target,double seconds,double engine_hz,double baseline_hz){
  std::fprintf(stderr,"preview cadence scope=%s policy20ms=%d policy120ms=%d sample_s=%.6f slow500ms=%zu engine_count=%zu engine_hz=%.3f engine_gap_p99_ms=%.3f baseline8_count=%zu baseline8_hz=%.3f baseline8_gap_p99_ms=%.3f supplies=%zu/%zu supply_hz=%.3f\n",
   require_native_target?"native120":"functional-clock-comparison",slow_interval,fast_interval,seconds,slow,engine_clock.count,engine_hz,engine_clock.p99_gap(),baseline_clock.count,baseline_hz,baseline_clock.p99_gap(),slow_supplies,supplies.load()-slow_supplies,(supplies.load()-slow_supplies)/seconds);
 }
 void verify(bool require_native_target){
  const double seconds=double(sample_end-sample_start)/1e9;
  const double engine_hz=engine_clock.count/seconds,baseline_hz=baseline_clock.count/seconds;
  report(require_native_target,seconds,engine_hz,baseline_hz);
  require(!bad_feed&&slow_supplies>100&&supplies.load()-slow_supplies>200,"preview cadence producer was not admitted above requested rate");
  require(slow>=5&&slow<=15,"preview cadence ignored cold20Hz policy");
  require(slow_taps>2*slow,"GUI cadence throttled loss-sensitive tap delivery");
  require(slow_interval==50&&fast_interval==8,"typed cadence did not configure requested Qt timer policy");
  require(baseline_hz>30,"platform clock baseline did not progress sufficiently to evaluate delivery");
  require(engine_hz>=baseline_hz*.85&&engine_hz<=baseline_hz*1.25+4,"preview delivery diverges from same-worker8ms precise timer baseline");
  if(require_native_target)require(engine_hz>=114&&baseline_hz>=114,"native worker delivery or platform clock did not sustain95percent of120Hz");
 }
};
inline void preview_cadence(bool require_native_target=false){CadenceScenario scenario;scenario.prepare();scenario.start();scenario.fast();scenario.finish(require_native_target);scenario.verify(require_native_target);}
}
