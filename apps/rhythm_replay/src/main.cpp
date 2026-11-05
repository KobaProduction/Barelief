#include <rhythm_replay/replay.h>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QTextStream>

namespace {
rhythm_replay::Options options(QCoreApplication& app) {
    QCommandLineParser parser;parser.setApplicationDescription("Replay audio through Barelief's normalized graph without playback or labels");
    parser.addHelpOption();
    parser.addOptions({{{"i","input"},"Audio file","file"},{{"o","out"},"New or empty output directory","directory"},
        {"packet-samples","PCM packet size (maximum 4096)","samples","256"},{"analysis-rate","Analysis rate: 30, 45, 60","Hz","60"},
        {"reference-trace","Export Float32 PCM/FFT/power/Mel/dB for numerical reference comparisons"}});
    parser.process(app);
    bool packet_valid=false,rate_valid=false;
    const auto packet=parser.value("packet-samples").toULongLong(&packet_valid);
    const auto rate=parser.value("analysis-rate").toUInt(&rate_valid);
    if(!packet_valid || !rate_valid || !parser.isSet("input") || !parser.isSet("out"))parser.showHelp(2);
    return {parser.value("input"),parser.value("out"),std::size_t(packet),rate,parser.isSet("reference-trace")};
}
}
int main(int argc,char** argv) {
    QCoreApplication app(argc,argv);
    const auto result=rhythm_replay::run(options(app));
    if(!result.success){QTextStream(stderr)<<result.error<<'\n';return 1;}
    QTextStream(stdout)<<"Replay complete: "<<result.input_samples<<" samples, "<<result.analysis_frames<<" analysis frames\n";
    return 0;
}
