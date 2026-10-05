#include "pipeline/audio_pipeline.h"
#include <thread>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QFile>
#include <QProcess>
#include <QDebug>
#include <QRegularExpression>
#include <QProcessEnvironment>
#include <stdexcept>
static void require(bool value,const char *message) { if(!value) throw std::runtime_error(message); }
static QJsonObject edge(int from,int to) { return {{"outNodeId",from},{"outPortIndex",0},{"inNodeId",to},{"inPortIndex",0}}; }
int main(int argc,char **argv) {
    QCoreApplication app(argc,argv);
    try {
        auto document=pipeline::GraphProgram::defaults(); QString error;
        auto program=pipeline::GraphProgram::compile(document,error); require(bool(program),qPrintable(error));
        auto legacy=document; auto legacy_nodes=legacy.value("nodes").toArray();
        for (int i=0;i<legacy_nodes.size();++i) {
            auto node=legacy_nodes[i].toObject(); auto parameters=node.value("internal-data").toObject();
            if (parameters.value("model-name")=="SpectrogramView") { parameters["model-name"]="SurfaceView"; node["internal-data"]=parameters; legacy_nodes[i]=node; }
        }
        legacy["nodes"]=legacy_nodes;
        const auto legacy_program=pipeline::GraphProgram::compile(legacy,error);
        require(legacy_program && legacy_program->contains("SpectrogramView") &&
            legacy_program->document()==document,"Legacy visualization node was not migrated");
        SpectrumFrame frame; frame.sample_rate=48000; frame.left.resize(2048); frame.right.resize(2048); frame.left[64]=1; frame.right[128]=2;
        auto output=program->evaluate(frame);
        require(output.fft[64]==1 && output.fft[128]==0 && output.mel.size()==50 && output.history.size()==50 && output.spectrogram,"Default routing incorrect");
        require(output.peak_hz==1500,"Merged peak ignores right channel");
        pipeline::GraphEvaluationContext context;
        for (int iteration=0;iteration<16;++iteration) {
            bool matched=false;
            // Reuse storage across joined workers, without owning thread-local data.
            std::thread worker([&] {
                const auto evaluated=program->evaluate(frame,context);
                matched=evaluated.history==output.history && evaluated.fft==output.fft;
            });
            worker.join();
            require(matched,"Joined worker changed graph output");
        }
        auto smaller=frame;smaller.sample_rate=44100;smaller.left.resize(1024);smaller.right.resize(1024);
        require(program->evaluate(smaller,context).mel==program->evaluate(smaller).mel,
                "Reused context retained an obsolete Mel filter");
        require(program->evaluate(frame,context).mel==output.mel,"Context did not restore the original Mel shape");
        auto microphone=pipeline::GraphProgram::defaults(0,SpectrumMerge::Energy,1);
        require(pipeline::GraphProgram::compile(microphone,error)->capture_source()==1,"Microphone source was not compiled");
        auto bad_source=pipeline::GraphProgram::defaults(0,SpectrumMerge::Energy,2);
        require(!pipeline::GraphProgram::compile(bad_source,error),"Invalid source accepted");
        auto source_nodes=microphone["nodes"].toArray(); auto source_node=source_nodes[0].toObject();
        auto source_params=source_node["internal-data"].toObject();source_params["source"]="microphone";
        source_node["internal-data"]=source_params;source_nodes[0]=source_node;microphone["nodes"]=source_nodes;
        require(!pipeline::GraphProgram::compile(microphone,error),"Malformed source parameter silently accepted");
        auto nodes=document["nodes"].toArray();
        nodes.append(QJsonObject{{"id",10},{"internal-data",QJsonObject{{"model-name","SpectrumGain"},{"gain",0}}}});
        auto edges=document["connections"].toArray();
        for(int i=0;i<edges.size();++i) if(edges[i].toObject()["inNodeId"].toInt()==3) edges[i]=edge(10,3);
        edges.append(edge(2,10)); document["nodes"]=nodes; document["connections"]=edges;
        auto muted=pipeline::GraphProgram::compile(document,error); require(bool(muted),qPrintable(error));
        auto result=muted->evaluate(frame); require(std::all_of(result.history.begin(),result.history.end(),[](float v){return v==0;}),"Gain node does not affect processing");
        require(muted->evaluate(frame,context).history==result.history,"Graph change retained stale scratch values");
        require(program->evaluate(frame,context).history==output.history,"Reused context did not restore the previous graph");
        pipeline::AudioPipeline engine; require(engine.apply_graph(document,error),"Valid graph rejected");
        auto active=engine.graph(); auto revision=engine.graph_revision();
        auto moved=active; nodes=moved["nodes"].toArray(); auto node=nodes[0].toObject(); node["position"]=QJsonObject{{"x",123},{"y",456}};nodes[0]=node;moved["nodes"]=nodes;
        require(engine.apply_graph(moved,error) && engine.graph_revision()==revision,"Moving nodes resets processing");
        auto missing=active; edges=missing["connections"].toArray();edges.removeAt(0);missing["connections"]=edges;
        require(!engine.apply_graph(missing,error) && engine.graph()==moved,"Invalid draft replaced active graph");
        auto bad_type=active;edges=bad_type["connections"].toArray();edges[0]=edge(3,1);bad_type["connections"]=edges;
        require(!pipeline::GraphProgram::compile(bad_type,error),"Incompatible ports accepted");
        auto cycle=active;edges=cycle["connections"].toArray();edges[edges.size()-1]=edge(10,10);cycle["connections"]=edges;
        require(!pipeline::GraphProgram::compile(cycle,error) && error.contains("cycle"),"Cycle accepted or misreported");
        auto unknown=active;unknown["version"]=99;require(!pipeline::GraphProgram::compile(unknown,error),"Unknown version accepted");
        auto bad_gain=active;nodes=bad_gain["nodes"].toArray();node=nodes.last().toObject();auto params=node["internal-data"].toObject();params["gain"]=-1;node["internal-data"]=params;nodes[nodes.size()-1]=node;bad_gain["nodes"]=nodes;
        require(!pipeline::GraphProgram::compile(bad_gain,error),"Invalid parameter accepted");
        QTemporaryDir temp; QFile file(temp.filePath("graph.json"));require(file.open(QIODevice::WriteOnly),"Cannot save graph");file.write(QJsonDocument(active).toJson());file.close();
        require(file.open(QIODevice::ReadOnly),"Cannot reload graph");auto loaded=QJsonDocument::fromJson(file.readAll()).object();file.close();
        require(loaded==active && pipeline::GraphProgram::compile(loaded,error)->evaluate(frame).history==result.history,"Graph roundtrip changed output");
        QProcess console; console.start(QCoreApplication::applicationDirPath()+"/pipeline_cli",{"--config",temp.filePath("graph.json"),"--synthetic","--duration","300"});
        require(console.waitForFinished(5000) && console.exitCode()==0,"Saved graph failed in console");
        QProcess cadence;auto env=QProcessEnvironment::systemEnvironment();env.insert("QT_FORCE_STDERR_LOGGING","1");cadence.setProcessEnvironment(env);
        cadence.start(QCoreApplication::applicationDirPath()+"/pipeline_cli",{"--synthetic","--duration","1200"});
        require(cadence.waitForFinished(7000) && cadence.exitCode()==0,"Synthetic cadence console failed");
        const auto report=QString::fromUtf8(cadence.readAllStandardError()+cadence.readAllStandardOutput());
        const auto count=QRegularExpression("Processed (\\d+) spectrum frames").match(report);
        require(count.hasMatch() && count.captured(1).toInt()>=50 && count.captured(1).toInt()<=90,"Console PCM cadence depends on timer wake frequency");
        qInfo("Graph validation, routing, atomic apply, persistence and console tests passed");
    } catch(const std::exception &error) { qCritical("Graph test: %s",error.what());return 1; }
}
