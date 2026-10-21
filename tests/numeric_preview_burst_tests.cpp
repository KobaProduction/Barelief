#include "numeric_transport_fixture.h"
#include <QCoreApplication>
#include <vector>
using namespace transport_fixture;
struct Sample {EndpointIdentity endpoint;std::uint64_t value;};
static void wide_cohort(NumericEngine& engine,Replies& replies,std::vector<Sample>& seen) {
  // A larger cohort falls back to one latest snapshot per endpoint. Smoothing
  // a small audio burst must not starve the first ports in a wide selection.
  auto wide=graph("wide-preview");wide.nodes.clear();wide.edges.clear();
  std::vector<EndpointIdentity> endpoints;
  auto prototype=graph();
  for(int i=0;i<65;++i){auto input=prototype.nodes[0],output=prototype.nodes[1];
   input.id=2*i;output.id=2*i+1;wide.nodes.push_back(input);wide.nodes.push_back(output);
   wide.edges.push_back({input.id,output.id,0,0});endpoints.push_back({wide.graph_id,output.id,domain::PortDirection::Input,0});}
  replies.accepted(apply(wide));require(engine.subscribe_numeric_frames(endpoints),"subscribe wide cohort");settle();seen.clear();
  for(int i=0;i<65;++i){feed(engine,1,1,301,2*i);feed(engine,1,2,302,2*i);}
  require(wait([&]{return seen.size()>=65;}),"wide cohort starved endpoints");
  for(const auto& endpoint:endpoints){std::size_t count=0;for(const auto& x:seen)if(x.endpoint==endpoint){++count;require(x.value==302,"wide cohort retained obsolete snapshot");}
   require(count==1,"wide cohort missing or duplicated endpoint");}
}

static void cadence_and_retirement(NumericEngine& engine,std::vector<Sample>& seen) {
  seen.clear();
  for(std::uint64_t i=103;i<=106;++i)feed(engine,1,i,100+i);
  engine.set_numeric_preview_cadence_hz(60);
  require(wait([&]{return seen.size()>=2;}),"cadence change stranded latest snapshots");
  for(const auto& x:seen)require(x.value==206,"slower cadence retained obsolete burst backlog");
  settle(60);require(seen.size()==2,"slower cadence replayed old burst window");
  engine.set_numeric_preview_cadence_hz(240);seen.clear();
  const auto retiredCount=seen.size();
  // Retiring demand must cancel both queued snapshots immediately.
  feed(engine,1,107,207);feed(engine,1,108,208);
  require(engine.subscribe_numeric_frames({}),"unsubscribe burst endpoints");settle(30);
  require(seen.size()==retiredCount,"retired subscription delivered pending burst");
}

static void bounded_overload(NumericEngine& engine,const auto& g,const EndpointIdentity& source,std::vector<Sample>& seen) {
  seen.clear();
  for(std::uint64_t i=3;i<=102;++i)feed(engine,1,i,100+i);
  require(wait([&]{return seen.size()>=8;}),"overload latest snapshots missing");
  for(const auto& endpoint:{source,sink(g)}){
   std::vector<Sample> lane;for(const auto& x:seen)if(x.endpoint==endpoint)lane.push_back(x);
   require(lane.size()==4,"preview backlog is unbounded or truncated below burst budget");
   for(std::size_t i=0;i<lane.size();++i){require(lane[i].value==199+i,"oldest rather than latest burst window retained");
   }
  }
  const auto count=seen.size();settle(30);require(seen.size()==count,"idle preview repeated stale snapshots");
}

int main(int argc,char** argv){
 QCoreApplication app(argc,argv);
 try {
  NumericEngine engine;const auto g=graph();Replies replies(engine);replies.accepted(apply(g));
  const EndpointIdentity source{g.graph_id,0,domain::PortDirection::Output,0};
  require(engine.subscribe_numeric_frames({source,sink(g)}),"subscribe burst endpoints");
  engine.set_numeric_preview_cadence_hz(240);settle();
  std::vector<Sample> seen;
  engine.set_numeric_frame_handler([&](const auto& f){if(f.frame && f.frame.view().stamp.available){const auto v=value(f);if(v>=100)seen.push_back({f.context->endpoint,v});}});
  // Two processing results in one callback must be offered on separate GUI
  // deadlines, rather than coalesced before the first owner event runs.
  feed(engine,1,1,101);feed(engine,1,2,102);
  require(wait([&]{return seen.size()>=4;}),"adjacent burst snapshot lost before presentation");
  for(const auto& endpoint:{source,sink(g)}){
   std::vector<Sample> lane;for(const auto& x:seen)if(x.endpoint==endpoint)lane.push_back(x);
   require(lane.size()==2 && lane[0].value==101 && lane[1].value==102,"burst order or endpoint fairness lost");
  }
  bounded_overload(engine,g,source,seen);
  cadence_and_retirement(engine,seen);
  wide_cohort(engine,replies,seen);
  engine.set_numeric_frame_handler({});
  std::puts("numeric_preview_burst_tests PASS: burst order, paced endpoints, bounded overload, idle, unsubscribe, 65-endpoint fairness");
  return 0;
 }catch(const std::exception& e){std::fprintf(stderr,"numeric_preview_burst_tests FAIL: %s\n",e.what());return 1;}
}
