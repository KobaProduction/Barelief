#include "synthetic_capture.h"
#include <control/numeric_desktop_host.h>
#include <core_app/numeric_default_graph.h>
#include <desktop/numeric_project_document.h>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QJsonDocument>
#include <cstdio>

int main(int argc,char** argv){
 QCoreApplication app(argc,argv);
 pipeline::NumericDesktopHost host(test_fixture::synthetic_capture_factory());
 const pipeline::EndpointIdentity source{"main",1,domain::PortDirection::Output,0},sink{"main",9,domain::PortDirection::Input,0};
 unsigned source_frames=0,sink_frames=0,snapshots=0;bool failed=false;
 host.set_numeric_frame_handler([&](const auto& frame){if(!frame.context||!frame.frame||!frame.frame.view().stamp.available)return;
  if(frame.context->endpoint==source)++source_frames;if(frame.context->endpoint==sink)++sink_frames;});
 // A view renews the same demand during snapshot projection. Renewal must not
 // retire already queued frames or trigger an endless full-history resync.
 host.set_snapshot_handler([&](const auto&){++snapshots;QString error;failed|=!host.subscribe_numeric_frames({sink,source,source},&error);});
 QByteArray bytes;if(!desktop::encode_numeric_graph(numeric_test_graph(),bytes))return 2;
 pipeline::EngineCommand apply;apply.kind=pipeline::EngineCommand::ApplyGraph;apply.graph=QJsonDocument::fromJson(bytes).object();host.submit(apply);
 auto wait=[&](const auto& predicate){QElapsedTimer t;t.start();while(t.elapsed()<4000){QCoreApplication::processEvents();if(predicate())return true;QThread::msleep(1);}return predicate();};
 if(!wait([&]{return host.snapshot().graph_revision>0;}))return 1;
 const bool delivered=wait([&]{return source_frames>=5&&sink_frames>=5;});
 const bool stopped=host.snapshot().statuses.value("analyzer").state==pipeline::State::Running;
 std::printf("same normalized subscription renewal: snapshots=%u source=%u sink=%u %s\n",snapshots,source_frames,sink_frames,delivered&&stopped&&!failed?"PASS":"FAIL");
 return delivered&&stopped&&!failed?0:1;
}
