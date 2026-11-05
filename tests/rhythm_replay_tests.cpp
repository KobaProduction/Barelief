#include <rhythm_replay/replay.h>
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <stdexcept>

namespace {
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
QByteArray contents(const QString& path) {
    QFile file(path);require(file.open(QIODevice::ReadOnly),"Read replay evidence");return file.readAll();
}
void wave_header(QDataStream& out,unsigned rate,unsigned samples) {
    out.writeRawData("RIFF",4);out<<quint32(36+2*samples);out.writeRawData("WAVEfmt ",8);
    out<<quint32(16)<<quint16(1)<<quint16(1);
    out<<quint32(rate)<<quint32(rate*2)<<quint16(2)<<quint16(16);
    out.writeRawData("data",4);out<<quint32(samples*2);
}
float test_sample(unsigned sample,unsigned rate) {
    const double time=double(sample)/rate;
    const double envelope=sample%(rate/2)<rate/16?1.:.05;
    double value=0;
    for(unsigned tone=0;tone<8;++tone)
        value+=.02*envelope*std::sin(2*std::numbers::pi*((tone+1)*167.+7*std::sin(time))*time+.13*tone);
    return float(value);
}
QString write_audio(const QString& root,unsigned rate,unsigned samples,bool silent=false) {
    const auto path=root+"/input.wav";QFile file(path);require(file.open(QIODevice::WriteOnly),"Write test WAV");
    QDataStream stream(&file);stream.setByteOrder(QDataStream::LittleEndian);wave_header(stream,rate,samples);
    for(unsigned i=0;i<samples;++i)stream<<qint16(silent?0:std::lround(test_sample(i,rate)*32767));
    require(stream.status()==QDataStream::Ok,"Write complete test PCM");return path;
}
QByteArray event_observations(const QString& root) {
    QByteArray result;
    const auto lines=contents(root+"/events.csv").split('\n');
    for(const auto& line:lines){
        if(line.isEmpty() || line.startsWith("stream,"))continue;
        auto fields=line.split(',');require(fields.size()==7,"Event schema");fields.removeAt(4);
        result+=fields.join(',')+'\n';
    }
    return result;
}
QByteArray reference_observations(const QString& root,bool silent=false) {
    QByteArray result;
    const auto lines=contents(root+"/reference_envelopes.csv").split('\n');
    for(const auto& line:lines){
        if(line.isEmpty() || line.startsWith("method,"))continue;
        auto fields=line.split(',');require(fields.size()==11,"Reference envelope CSV schema");
        require(fields[2].toUInt()==44100 && fields[4].toUInt()==2048,"Reference sample clock and complete FFT window");
        require(fields[3].toLongLong()==qint64(fields[1].toULongLong()-1)*512,"Fixed hop is independent of packet and baseline cadence");
        require(fields[8].toDouble()>=fields[7].toDouble(),"Source readiness never precedes window end");
        const auto value=fields[10].toDouble();require(std::isfinite(value) && (!silent || value==0),"Finite raw envelopes and silent zeros");
        fields.removeAt(8);result+=fields.join(',')+'\n';
    }
    return result;
}
void verify_reference_trace(const QString& first,const QString& second) {
    const auto summary=QJsonDocument::fromJson(contents(first+"/summary.json")).object().value("reference").toObject();
    require(summary.value("hop_samples").toInt()==512 && !summary.value("center").toBool(),"Recorded native causal configuration");
    const auto samples=summary.value("pcm_samples").toInt(),frames=summary.value("frames").toInt();
    require(frames==1+(samples-2048)/512 && summary.value("methods").toArray().size()==6,"Six native descriptors per complete window");
    const auto stages=summary.value("trace").toArray();require(stages.size()==5,"Explicit optional intermediate trace");
    for(const auto& item:stages){
        const auto stage=item.toObject();const auto name=stage.value("file").toString();
        const auto data=contents(first+'/'+name);
        require(data.size()==stage.value("frames").toInteger()*stage.value("width").toInteger()*4,"Binary Float32 dimensions and no text header");
        require(data==contents(second+'/'+name),"Complete reference PCM/FFT/Mel/dB is invariant to packet partitions");
    }
    require(reference_observations(first)==reference_observations(second),"Native descriptors and source times are packet invariant");
}
void verify_fourier_phases(const QString& directory,bool silent=false) {
    const auto magnitudes=contents(directory+"/tempograms.csv").split('\n');
    const auto phases=contents(directory+"/fourier_phases.csv").split('\n');
    require(phases.size()==magnitudes.size() && phases[0].endsWith("phase_rad"),"Every exported magnitude has a matched explicit phase");
    bool nonzero=false;
    for(qsizetype row=1;row<phases.size();++row){
        if(phases[row].isEmpty())continue;
        const auto phase=phases[row].split(','),magnitude=magnitudes[row].split(',');
        require(phase.size()==7 && magnitude.size()==7,"Phase CSV schema");
        for(int field=0;field<6;++field)require(phase[field]==magnitude[field],"Phase shares source timestamp, occupancy, bin and Hz");
        const auto value=phase[6].toDouble();
        require(std::isfinite(value) && std::abs(value)<=3.141593,"Finite phase in radians");
        if(silent || magnitude[6].toDouble()==0)require(value==0,"Zero complex bin has zero phase");
        nonzero=nonzero || std::abs(value)>.01;
    }
    require(silent || nonzero,"Changing tones exercise nontrivial Fourier phase");
}
void packet_partition_and_short_tail() {
    QTemporaryDir temporary;require(temporary.isValid(),"Temporary replay fixture");
    constexpr unsigned rate=44100,samples=3*rate+17;
    const auto audio=write_audio(temporary.path(),rate,samples);
    const auto first=temporary.path()+"/packet256",second=temporary.path()+"/packet64";
    const auto a=rhythm_replay::run({audio,first,1024,60,true}),b=rhythm_replay::run({audio,second,64,60,true});
    if(!a.success || !b.success)std::fprintf(stderr,"replay errors: %s / %s\n",qPrintable(a.error),qPrintable(b.error));
    require(a.success && b.success,"Both packet partitions complete");
    require(a.input_samples==samples && b.input_samples==samples,"Short final packet is retained without padding");
    require(a.source_rate==rate && b.source_rate==rate,"Native Fs is explicit");
    if(a.analysis_frames!=180 || b.analysis_frames!=180)
        std::fprintf(stderr,"Replay cadence mismatch: stream=compact_float:1007 expected=180 formula=floor(%u*60/%u)=%llu packet_a=1024 actual_a=%llu input_a=%llu Fs_a=%u packet_b=64 actual_b=%llu input_b=%llu Fs_b=%u\n",
            samples,rate,static_cast<unsigned long long>(std::uint64_t(samples)*60/rate),
            static_cast<unsigned long long>(a.analysis_frames),static_cast<unsigned long long>(a.input_samples),a.source_rate,
            static_cast<unsigned long long>(b.analysis_frames),static_cast<unsigned long long>(b.input_samples),b.source_rate);
    require(a.analysis_frames==180 && b.analysis_frames==180,"Only complete cadence deadlines produce frames");
    verify_reference_trace(first,second);
    require(contents(first+"/features.csv")==contents(second+"/features.csv"),"Shared frontend values and source times are packet invariant");
    require(contents(first+"/tempograms.csv")==contents(second+"/tempograms.csv"),"Production Fourier snapshots are packet invariant");
    require(contents(first+"/fourier_phases.csv")==contents(second+"/fourier_phases.csv"),"Fourier phase and its source time are packet invariant");
    require(contents(first+"/correlations.csv")==contents(second+"/correlations.csv"),"Linear correlation snapshots are packet invariant");
    require(contents(first+"/tempo_candidates.csv")==contents(second+"/tempo_candidates.csv"),"Refined evidence is packet invariant");
    require(event_observations(first)==event_observations(second),"Onsets are packet invariant; readiness is separate");
    require(!event_observations(first).isEmpty(),"Eight changing tones exercise detected attacks");
    verify_fourier_phases(first);
}
void verify_silent_correlations(const QString& directory) {
    const auto lines=contents(directory+"/correlations.csv").split('\n');unsigned rows=0;
    for(const auto& line:lines){
        if(line.isEmpty() || line.startsWith("window,"))continue;
        const auto fields=line.split(',');require(fields.size()==7,"Correlation CSV schema");
        require(fields[6].toDouble()==0,"Silent ACF has no support");
        require(std::abs(fields[5].toDouble()-fields[4].toDouble()/45)<1e-12,"Explicit 45 Hz ACF lag coordinates");
        require(fields[3].toUInt()==fields[1].toUInt(),"ACF occupancy paired by matching sequence");++rows;
    }
    require(rows==7*2*121,"First and every fifteenth ACF frame exported");
}
void silence_and_explicit_cadence() {
    QTemporaryDir temporary;require(temporary.isValid(),"Silence fixture");
    constexpr unsigned rate=48000,samples=rate*2+37;
    const auto audio=write_audio(temporary.path(),rate,samples,true),out=temporary.path()+"/result";
    const auto result=rhythm_replay::run({audio,out,127,45});
    if(!result.success)std::fprintf(stderr,"silence error: %s\n",qPrintable(result.error));
    require(result.success && result.input_samples==samples && result.analysis_frames==90,"45 Hz cadence and EOF");
    require(event_observations(out).isEmpty(),"Silence creates no attacks");
    const auto summary=QJsonDocument::fromJson(contents(out+"/summary.json")).object();
    require(!reference_observations(out,true).isEmpty(),"Silent native branch is observed alongside the 45 Hz baseline");
    require(!QFile::exists(out+"/reference_pcm.f32"),"Intermediate binary traces are opt-in");
    require(summary["analysis_rate"].toInt()==45 && summary["source_rate"].toInt()==48000,"Recorded declared clocks");
    const auto streams=summary["streams"].toArray();
    for(const auto& stream:streams){
        const auto entry=stream.toObject();require(entry["frames"].toInt()==90 && entry["maximum"].toDouble()==0,"Every silence stream is valid zero");
    }
    const auto lines=contents(out+"/tempograms.csv").split('\n');unsigned rows=0;
    for(const auto& line:lines){
        if(line.isEmpty() || line.startsWith("window,"))continue;
        const auto fields=line.split(',');require(fields.size()==7,"Tempogram CSV schema");
        const auto size=fields[0].toUInt();const auto bin=fields[4].toUInt();++rows;
        require(bin<=size/2 && fields[6].toDouble()==0 && std::abs(fields[5].toDouble()-double(bin)*45/size)<1e-12,"Silent Fourier includes Nyquist at the explicit 45 Hz analysis rate");
        require(fields[3].toUInt()==fields[1].toUInt(),"Short history records actual occupancy");
    }
    require(rows==7*(257+2049),"First and every fifteenth complete nonnegative Fourier row exported");
    verify_silent_correlations(out);
    verify_fourier_phases(out,true);
}
void float_analyzer_reuses_the_same_frontend() {
    QTemporaryDir temporary;require(temporary.isValid(),"Float analyzer fixture");
    const auto audio=write_audio(temporary.path(),44100,44100*3+17);
    const auto linear=temporary.path()+"/float";
    const auto result=rhythm_replay::run({audio,linear});
    require(result.success && result.analysis_frames==180,"Canonical linear analyzer history executes");
    const auto summary=QJsonDocument::fromJson(contents(linear+"/summary.json")).object();
    require(summary["analyzer_input"].toString()=="float32-linear","Recorded direct Float32 path");
    const auto streams=summary["streams"].toArray();bool bounded=false;
    for(const auto& value:streams){
        const auto stream=value.toObject();if(stream["stream"].toString()!="compact_float")continue;
        bounded=stream["minimum"].toDouble()>=0 && std::isfinite(stream["maximum"].toDouble());
    }
    require(bounded && streams.size()==3,"Unquantized streams are finite and retain headroom");
    const auto nodes=QJsonDocument::fromJson(contents(linear+"/graph.json")).object()["nodes"].toArray();
    unsigned found=0;
    for(const auto& value:nodes){
        const auto node=value.toObject();const auto id=node["id"].toInt();
        require(node["kind"].toString()!="SpectralQuantize" && id!=1112 && id!=1113,"No quantization/restoration in analysis graph");
        if(id!=7 && id!=1302)continue;
        const auto port=node["outputs"].toArray().at(0).toObject();++found;
        require(port["contract"].toObject()["dtype"].toString()=="Float32","Both feature histories retain Float32");
    }
    require(found==2,"Both prepared histories are present");
}
void maximum_packet_and_cadence_capacity() {
    QTemporaryDir temporary;require(temporary.isValid(),"Maximum packet cadence fixture");
    constexpr unsigned rate=44100,samples=rate+17;
    const auto audio=write_audio(temporary.path(),rate,samples);
    const auto result=rhythm_replay::run({audio,temporary.path()+"/result",4096,30});
    if(!result.success)std::fprintf(stderr,"maximum packet replay error: %s\n",qPrintable(result.error));
    require(result.success,"Maximum packet and slow cadence execute without FIFO loss or underrun");
    require(result.input_samples==samples && result.source_rate==rate && result.analysis_frames==30,"Maximum packet preserves real EOF and complete 30 Hz deadlines");
}
void refined_candidates_from_production_history() {
    QTemporaryDir temporary;require(temporary.isValid(),"Refined evidence fixture");
    const auto audio=write_audio(temporary.path(),44100,12*44100+17),out=temporary.path()+"/result";
    const auto result=rhythm_replay::run({audio,out});
    if(!result.success)std::fprintf(stderr,"tempo replay error: %s\n",qPrintable(result.error));
    require(result.success,"Twelve second production history executes");
    const auto summary=QJsonDocument::fromJson(contents(out+"/summary.json")).object()["tempo_evidence"].toObject();
    require(summary["frames"].toInt()==int(result.analysis_frames) && summary["source"].toString().startsWith("production TempoEvidence"),"Replay records graph candidate outputs on every analysis frame");
    const auto lines=contents(out+"/tempo_candidates.csv").split('\n');bool warmup=false,found=false;
    for(const auto& line:lines){
        if(line.isEmpty() || line.startsWith("frame,"))continue;
        const auto fields=line.split(',');require(fields.size()==15,"Refined evidence schema");
        const auto time=fields[1].toDouble();const bool available=fields[4].toUInt()!=0;
        if(fields[2].toUInt()<240 && fields[3].toUInt()<240){
            require(!available,"Explicit real-sample warmup cannot publish a candidate");warmup=true;
        }
        if(!available)continue;
        const auto bpm=fields[6].toDouble(),score=fields[7].toDouble();
        require(fields[5].toUInt()<8 && bpm>=40 && bpm<=240 && score>=0 && score<=1,"Bounded candidate values");
        found=found || (time>=8 && std::abs(bpm/120-1)<.02);
    }
    require(warmup && found,"Same eight tone frontend retains the 120 BPM modulation hypothesis");
}
void refuses_existing_output_and_invalid_input() {
    QTemporaryDir temporary;require(temporary.isValid(),"Protected output fixture");
    const auto audio=write_audio(temporary.path(),48000,1024),out=temporary.path()+"/existing";
    require(QDir().mkpath(out),"Create protected fixture");
    QFile sentinel(out+"/.protected");require(sentinel.open(QIODevice::WriteOnly),"Create sentinel");sentinel.write("original");sentinel.close();
    const auto result=rhythm_replay::run({audio,out});
    require(!result.success && result.error.contains("empty"),"Existing hidden content refuses overwrite");
    require(contents(sentinel.fileName())=="original" && !QFile::exists(out+"/events.csv"),"Existing output preserved");
    const auto invalid=rhythm_replay::run({audio,temporary.path()+"/invalid",0});
    require(!invalid.success && !QDir(temporary.path()+"/invalid").exists(),"Invalid options do not create output");
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    try{
        if(app.arguments().contains("--tempo-evidence")){refined_candidates_from_production_history();return 0;}
        packet_partition_and_short_tail();maximum_packet_and_cadence_capacity();silence_and_explicit_cadence();float_analyzer_reuses_the_same_frontend();refuses_existing_output_and_invalid_input();return 0;
    }
    catch(const std::exception& error){std::fprintf(stderr,"FAIL rhythm replay: %s\n",error.what());return 1;}
}
