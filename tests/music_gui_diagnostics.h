#pragma once
#include <QElapsedTimer>
#include <QtGlobal>
#include <cstdint>
#include <cstdio>

namespace music_gui_diagnostics {
// Opt-in, GUI-thread-only telemetry; no worker or processing callback records here.
inline bool enabled() {
    static const bool value=qEnvironmentVariableIntValue("BARELIEF_MUSIC_GUI_DIAGNOSTICS")==1;
    return value;
}
struct Context {
    const char* stage="main";
    const char* parent="none";
    const char* field="none";
    std::uint64_t revision=0,iteration=0,rows=0,columns=0;
    long long filled=-1;
    int pending=-1,node=-1,edit_node=-1,status=-1,available=-1;
};
inline Context context;
class Recorder {
    QElapsedTimer clock_;
    std::uint64_t records_=0;
    static constexpr std::uint64_t maximum_records=200000;
public:
    Recorder(){if(enabled())clock_.start();}
    void event(const char* phase,long long value=-1) {
        if(!enabled() || records_>maximum_records)return;
        if(records_++==maximum_records){std::fputs("MUSIC_DIAG TRACE_LIMIT\n",stderr);std::fflush(stderr);return;}
        const auto& c=context;
        std::fprintf(stderr,"MUSIC_DIAG seq=%llu ms=%lld phase=%s stage=%s parent=%s value=%lld iteration=%llu observed_revision=%llu observed_pending=%d edit_node=%d field=%s last_node=%d last_status=%d last_available=%d last_rows=%llu last_columns=%llu last_filled=%lld\n",
            static_cast<unsigned long long>(records_),static_cast<long long>(clock_.elapsed()),phase,c.stage,c.parent,value,
            static_cast<unsigned long long>(c.iteration),static_cast<unsigned long long>(c.revision),c.pending,c.edit_node,c.field,c.node,c.status,c.available,
            static_cast<unsigned long long>(c.rows),static_cast<unsigned long long>(c.columns),c.filled);
        std::fflush(stderr);
    }
};
inline Recorder& recorder(){static Recorder value;return value;}
inline void event(const char* phase,long long value=-1){recorder().event(phase,value);}
class Scope {
    const char* stage_;
    const char* parent_;
public:
    explicit Scope(const char* stage):stage_(context.stage),parent_(context.parent) {
        if(!enabled())return;
        context.parent=context.stage;context.stage=stage;event("BEGIN");
    }
    ~Scope(){if(enabled()){event("END");context.stage=stage_;context.parent=parent_;}}
};
inline void edit(int node,const char* field,long long value=-1) {
    if(!enabled())return;
    context.edit_node=node;context.field=field;event("EDIT",value);
}
inline void iteration(std::uint64_t value) {if(enabled())context.iteration=value;}
inline void application(std::uint64_t revision,bool pending) {
    if(!enabled())return;
    context.revision=revision;context.pending=pending;event("APPLICATION");
}
inline void endpoint(int node,int status,bool available,std::uint64_t rows,std::uint64_t columns,long long filled) {
    if(!enabled())return;
    context.node=node;context.status=status;context.available=available;
    context.rows=rows;context.columns=columns;context.filled=filled;event("ENDPOINT");
}
} // namespace music_gui_diagnostics
