// SPDX-License-Identifier: AGPL-3.0-or-later
#include "offline_run.h"
#include "wave_input.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtEndian>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <vector>

namespace {
void check(bool value,const char* message) {
    if(!value){std::fprintf(stderr,"FAIL whole input: %s\n",message);std::exit(1);}
}
void append16(QByteArray& bytes,std::uint16_t value) {
    const auto little=qToLittleEndian(value);bytes.append(reinterpret_cast<const char*>(&little),sizeof(little));
}
void append32(QByteArray& bytes,std::uint32_t value) {
    const auto little=qToLittleEndian(value);bytes.append(reinterpret_cast<const char*>(&little),sizeof(little));
}
QByteArray wave_header(std::uint32_t samples,std::uint32_t rate) {
    QByteArray result("RIFF");append32(result,36+samples*4);result.append("WAVEfmt ");append32(result,16);
    append16(result,3);append16(result,1);append32(result,rate);append32(result,rate*4);
    append16(result,4);append16(result,32);result.append("data");append32(result,samples*4);return result;
}
std::vector<float> tones(std::size_t count,std::uint32_t rate) {
    std::vector<float> result(count);constexpr std::array frequencies{110.,251.,503.,997.,1999.,4001.,6007.,10007.};
    for(std::size_t i=0;i<count;++i){
        const auto t=double(i)/rate;const auto age=double(i%(rate/2))/rate;
        for(std::size_t k=0;k<frequencies.size();++k)
            result[i]+=float(.07*std::exp(-age/.006)*std::sin(6.283185307179586*frequencies[k]*t+.17*double(k)));
    }
    return result;
}
void wave(const QString& path,std::uint32_t count,std::uint32_t rate) {
    static_assert(std::endian::native==std::endian::little);
    QFile file(path);check(file.open(QIODevice::WriteOnly|QIODevice::NewOnly),"fresh WAV");
    const auto header=wave_header(count,rate);const auto values=tones(count,rate);
    check(file.write(header)==header.size(),"write WAV header");
    check(file.write(reinterpret_cast<const char*>(values.data()),qint64(count)*4)==qint64(count)*4,"write actual WAV samples");
}
QByteArray read(const QString& path) {
    QFile file(path);check(file.open(QIODevice::ReadOnly),"read actual runner evidence");return file.readAll();
}
QJsonObject report(const QString& directory) {
    return QJsonDocument::fromJson(read(QDir(directory).filePath("capture.json"))).object();
}
void verify(const QString& directory,std::size_t count,std::size_t collected,std::size_t packet) {
    const auto result=report(directory);
    check(result.value("execution_complete").toBool(),"complete whole graph execution");
    check(result.value("processed_samples").toInteger()==qint64(count),"all real PCM processed");
    check(result.value("collected_values").toInteger()==qint64(collected),"entire collection retained");
    check(result.value("packet_samples").toInteger()==qint64(packet),"requested real packet envelope");
    check(result.value("batch_attempts").toInteger()==1,"exactly one final batch");
    check(result.value("numeric_parity")=="not_evaluated","execution is separate from source numerical comparison");
    for(const auto& value:result.value("endpoints").toArray()){
        const auto endpoint=value.toObject();check(endpoint.value("complete").toBool(),"every required endpoint complete");
        check(endpoint.value("available_publications").toInteger()==1,"one final available publication");
        check(!endpoint.value("streaming_publications").toInteger(),"no rolling temporal result before EOF");
    }
}
void packet_parity(const QString& first,const QString& second) {
    for(const auto& value:report(first).value("endpoints").toArray()){
        const auto file=value.toObject().value("file").toString();
        check(read(QDir(first).filePath(file))==read(QDir(second).filePath(file)),"packet97/4096 final payload bit parity");
    }
}
void complete_input(const QDir& root,const QString& preset,std::uint32_t count,std::uint32_t rate) {
    const auto input=root.filePath(preset+".wav");wave(input,count,rate);
    const auto first=root.filePath(preset+"-97"),second=root.filePath(preset+"-4096");
    check(run_whole_input({preset,input,first,97})==0,"actual whole runner packet97");
    check(run_whole_input({preset,input,second,4096})==0,"actual whole runner packet4096");
    const auto collected=rate==44100?count:1+(count-2048)/240;
    verify(first,count,collected,97);verify(second,count,collected,4096);packet_parity(first,second);
}
void lifecycle() {
    WholeInputProgress state(101);
    check(!state.finish(true),"reject incomplete PCM before final publication");
    check(state.accept(97),"accept actual first packet");
    check(!state.accept(5) && state.processed()==97,"reject excess input atomically");
    check(state.accept(4),"accept exact actual tail");
    check(!state.finish(false) && !state.finalized(),"reject incomplete collector without sealing");
    check(state.finish(true),"seal complete PCM and collector");
    check(!state.finish(true) && !state.accept(1),"reject second EOF and input after EOF");
    WholeInputProgress empty(0);check(!empty.finish(true),"empty session does not fabricate a batch");
}
void publication_lifecycle() {
    using signal_contracts::Status;
    WholeOutputProgress initial;
    check(!initial.observe(Status::unavailable,false,false) && !initial.errors,"initial unavailable is a boundary only");
    check(initial.observe(Status::ok,true,true) && initial.complete(),"initial boundary followed by real final output");
    check(!initial.observe(Status::unavailable,false,true) && !initial.complete() && initial.errors==1,"late unavailable invalidates old successful payload");
    check(initial.observe(Status::ok,true,true) && !initial.complete(),"later output cannot erase latched invalidation");
    WholeOutputProgress empty;
    check(empty.observe(Status::ok,true,true) && empty.complete(),"available typed empty follows the successful publication path");
    WholeOutputProgress error;check(!error.observe(Status::invalid_argument,true,true) && !error.complete(),"error is never an empty result");
}
void rejected(const WholeInputArguments& args) {
    bool failed=false;try {failed=run_whole_input(args)!=0;}catch(const std::exception&){failed=true;}
    check(failed && !QFileInfo::exists(args.output),"cold rejection leaves no claimed output evidence");
}
void malformed_inputs(const QDir& root) {
    const auto truncated=root.filePath("truncated.wav");
    {QFile file(truncated);check(file.open(QIODevice::WriteOnly),"truncated fixture");file.write(wave_header(4096,44100));file.write(QByteArray(16,0));}
    rejected({"essentia-finite-degara",truncated,root.filePath("truncated-result"),97});
    const auto oversized=root.filePath("oversized.wav");
    {QFile file(oversized);check(file.open(QIODevice::WriteOnly),"oversize fixture");file.write(wave_header(4194305,44100));check(file.resize(44+4194305LL*4),"sparse PCM beyond explicit4M profile");}
    rejected({"essentia-finite-degara",oversized,root.filePath("oversized-result"),4096});
    const auto wrong_rate=root.filePath("wrong-rate.wav");wave(wrong_rate,4096,32000);
    rejected({"madmom-whole-superflux-crf",wrong_rate,root.filePath("rate-result"),4096});
    rejected({"madmom-reference-superflux-crf",wrong_rate,root.filePath("old-preset-result"),4096});
    const auto clamped=root.filePath("clamped-long-header.wav");
    {QFile file(clamped);check(file.open(QIODevice::WriteOnly),"long header truncated physical fixture");file.write(wave_header(514761,48000));file.write(QByteArray(4096*4,0));}
    rejected({"madmom-whole-superflux-crf",clamped,root.filePath("clamped-result"),97});
}
void rejected_container(const QDir& root,const QString& name,const QByteArray& contents) {
    const auto input=root.filePath(name+".wav");
    {QFile file(input);check(file.open(QIODevice::WriteOnly),"fresh malformed container");check(file.write(contents)==contents.size(),"write malformed container");}
    bool failed=false;try {(void)inspect_wave_input(input);}catch(const std::exception&){failed=true;}
    check(failed,"container rejected before decoder can clamp it");
}
QByteArray riff(const QByteArray& chunks) {
    QByteArray result("RIFF");append32(result,std::uint32_t(chunks.size()+4));result.append("WAVE");result.append(chunks);return result;
}
void container_boundaries(const QDir& root) {
    auto contents=wave_header(4096,48000);contents.append(QByteArray(4096*4,0));
    auto overrun=contents;const auto large=qToLittleEndian(std::uint32_t{0xffffffff});std::memcpy(overrun.data()+40,&large,sizeof(large));
    rejected_container(root,"chunk-overrun",overrun);
    rejected_container(root,"odd-padding",riff(QByteArray("JUNK\1\0\0\0x",9)));
    rejected_container(root,"short-chunk-header",riff(QByteArray("JUNK",4)));
    rejected_container(root,"duplicate-data",riff(contents.sliced(12)+contents.sliced(36)));
    auto extensible=contents;extensible[20]=char(0xfe);extensible[21]=char(0xff);
    rejected_container(root,"unsupported-extensible",extensible);
    const auto valid=root.filePath("padded-unknown.wav");
    {QFile file(valid);check(file.open(QIODevice::WriteOnly),"padded unknown chunk");file.write(riff(QByteArray("JUNK\1\0\0\0x\0",10)+contents.sliced(12)));}
    const auto metadata=inspect_wave_input(valid);check(metadata.frames==4096 && metadata.sample_rate==48000,"valid odd-padded unknown chunk retains declared PCM extent");
}
void incompatible_options(const QDir& root) {
    QCommandLineParser parser;add_whole_input_options(parser);
    parser.addOption({"preset","preset","id"});parser.addOption({"duration","duration","ms"});
    parser.addOption({"synthetic","synthetic"});parser.addOption({"port","port","number"});
    parser.addOption({"project","project","path"});parser.addOption({"export","export","path"});
    parser.addOption({"observe","observe","endpoint"});
    const auto output=root.filePath("incompatible");
    check(parser.parse({"test","--whole-input","--preset","essentia-finite-degara","--input","input.wav",
        "--output",output,"--duration","1"}),"parse valid mixed syntax");
    bool failed=false;try {(void)whole_input_arguments(parser);}catch(const std::exception&){failed=true;}
    check(failed && !QFileInfo::exists(output),"reject live duration before any capture or output");
}
void defined_empty(const QDir& root) {
    const auto input=root.filePath("short-silence.wav"),output=root.filePath("short-silence-result");
    {QFile file(input);check(file.open(QIODevice::WriteOnly),"short silence WAV");file.write(wave_header(2048,48000));file.write(QByteArray(2048*4,0));}
    check(run_whole_input({"madmom-whole-superflux-crf",input,output,97})==0,"defined short silence is successful typed empty");
    verify(output,2048,1,97);check(report(output).value("result")=="no_result","only available empty is no_result");
}
}
int main(int argc,char** argv) {
    QCoreApplication application(argc,argv);QTemporaryDir temporary;
    check(temporary.isValid(),"owned temporary evidence directory");const QDir root(temporary.path());
    if(argc>1 && std::strcmp(argv[1],"--no-result-only")==0){defined_empty(root);std::puts("Whole WAV no-result PASS");return 0;}
    if(argc>1 && std::strcmp(argv[1],"--positive-only")==0){
        complete_input(root,"essentia-finite-multifeature",396900,44100);
        complete_input(root,"madmom-whole-superflux-crf",514761,48000);
        std::puts("Whole WAV positive partition regression PASS");return 0;
    }
    lifecycle();publication_lifecycle();malformed_inputs(root);container_boundaries(root);incompatible_options(root);defined_empty(root);
    complete_input(root,"essentia-finite-multifeature",396900,44100);
    complete_input(root,"madmom-whole-superflux-crf",514761,48000);
    std::puts("Whole WAV input PASS");return 0;
}
