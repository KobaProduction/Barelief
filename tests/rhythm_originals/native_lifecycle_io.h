#pragma once
#include <array>
#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

namespace native_lifecycle {
#ifdef NATIVE_LIFECYCLE_REFERENCE
using Value=double;
#else
using Value=float;
#endif
enum class Component {Onset,Tempo,Odf,Tracker};
enum class Command {ClockReset=1,Restart,Geometry,OnsetControls,DefaultProfile,TempoControls,Method,SetTempo,FixTempo,ReleaseTempo};
struct Action {std::size_t before_hop=0;Command command{};std::array<double,8> values{};};
struct Plan {
    Component component{};
    std::size_t frame=512,hop=256,rate=44100,method=0,window=1;
    bool fresh_pair=false;
    std::vector<Action> actions;
};
struct Row {
    std::array<Value,684> values{};
    std::array<std::uint64_t,8> integers{};
    std::size_t value_count=0,integer_count=0;
};
struct Memory {std::size_t queried=0,owned=0;bool available=false;};
inline void require(bool value,const char* text) {if(!value)throw std::runtime_error(text);}
inline std::uint32_t unsigned_argument(double value) {
    require(std::isfinite(value) && value>=0 && value<=UINT32_MAX && std::floor(value)==value,"bounded unsigned lifecycle argument");
    return std::uint32_t(value);
}
inline std::int32_t signed_argument(double value) {
    require(std::isfinite(value) && value>=INT32_MIN && value<=INT32_MAX && std::floor(value)==value,"bounded signed lifecycle argument");
    return std::int32_t(value);
}
inline Action read_action(std::istream& input) {
    Action action;unsigned command=0;
    require(bool(input>>action.before_hop>>command) && command>=1 && command<=10,"lifecycle command");
    action.command=Command(command);
    for(auto& value:action.values)require(bool(input>>value) && std::isfinite(value),"finite lifecycle command payload");
    if(action.command==Command::Geometry){
        const auto hop=unsigned_argument(action.values[0]),frame=unsigned_argument(action.values[1]);
        require(hop && frame>=hop && frame<=65536,"bounded updated lifecycle geometry");
    }
    return action;
}
inline void validate_plan(const Plan& plan,unsigned component,std::size_t count) {
    require(component<=3 && count<=32 && plan.method<=9 && plan.window<=4,"bounded lifecycle selector/count");
    require(plan.frame && plan.frame<=65536 && plan.hop && plan.hop<=plan.frame && plan.rate && plan.rate<=UINT32_MAX,"bounded lifecycle plan geometry");
}
inline Plan read_plan(const std::filesystem::path& path) {
    std::ifstream input(path);Plan plan;unsigned component=0;std::size_t count=0;
    require(bool(input>>component>>plan.frame>>plan.hop>>plan.rate>>plan.method>>plan.window>>plan.fresh_pair>>count),"lifecycle plan header");
    validate_plan(plan,component,count);
    plan.component=Component(component);plan.actions.resize(count);
    for(auto& action:plan.actions)action=read_action(input);
    return plan;
}
inline std::vector<float> read_pcm(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary|std::ios::ate);require(bool(input),"lifecycle PCM input");
    const auto bytes=input.tellg();require(bytes>=0 && std::size_t(bytes)%sizeof(float)==0,"Float32 PCM geometry");
    std::vector<float> output(std::size_t(bytes)/sizeof(float));input.seekg(0);
    input.read(reinterpret_cast<char*>(output.data()),bytes);require(bool(input),"complete lifecycle PCM read");return output;
}
class Operation {
public:
    virtual ~Operation()=default;
    virtual bool apply(const Action&)=0;
    virtual bool process(std::span<const float>,Row&)=0;
    virtual std::size_t hop() const noexcept=0;
    virtual std::unique_ptr<Operation> fresh() const=0;
    virtual Memory memory() const noexcept {return {};}
};
std::unique_ptr<Operation> aubio(const Plan&);
std::unique_ptr<Operation> btrack(const Plan&);
class Output {
public:
    explicit Output(const std::filesystem::path& prefix):
        values_(prefix.string()+(sizeof(Value)==4?".values.f32":".values.f64"),std::ios::binary),
        integers_(prefix.string()+".integers.u64",std::ios::binary),fresh_(prefix.string()+".fresh.u64",std::ios::binary),
        memory_(prefix.string()+".memory.u64",std::ios::binary) {
        require(bool(values_) && bool(integers_) && bool(fresh_) && bool(memory_),"lifecycle output files");
    }
    void row(const Row& value,bool paired,bool equal,Memory memory) {
        values_.write(reinterpret_cast<const char*>(value.values.data()),std::streamsize(value.value_count*sizeof(Value)));
        integers_.write(reinterpret_cast<const char*>(value.integers.data()),std::streamsize(value.integer_count*sizeof(std::uint64_t)));
        const std::uint64_t status=paired?std::uint64_t(equal):2;
        fresh_.write(reinterpret_cast<const char*>(&status),sizeof(status));
        const std::array<std::uint64_t,3> fields{memory.queried,memory.owned,std::uint64_t(memory.available)};
        memory_.write(reinterpret_cast<const char*>(fields.data()),sizeof(fields));
        require(bool(values_) && bool(integers_) && bool(fresh_) && bool(memory_),"lifecycle output write");
    }
private:
    std::ofstream values_,integers_,fresh_,memory_;
};
inline bool identical(const Row& a,const Row& b) noexcept {
    return a.value_count==b.value_count && a.integer_count==b.integer_count &&
        std::memcmp(a.values.data(),b.values.data(),a.value_count*sizeof(Value))==0 &&
        std::memcmp(a.integers.data(),b.integers.data(),a.integer_count*sizeof(std::uint64_t))==0;
}
inline bool fresh_command(Command command) noexcept {return command==Command::Restart || command==Command::Geometry;}
inline void apply_at(const Plan& plan,std::size_t step,Operation& operation,std::unique_ptr<Operation>& fresh) {
    for(const auto& action:plan.actions)if(action.before_hop==step){
        require(operation.apply(action),"source or port lifecycle action rejected");
        if(fresh)require(fresh->apply(action),"fresh comparison action rejected");
        if(plan.fresh_pair && fresh_command(action.command))fresh=operation.fresh();
    }
}
inline void execute(const Plan& plan,std::span<const float> pcm,Operation& operation,Output& output) {
    std::unique_ptr<Operation> fresh;std::size_t position=0,step=0;
    while(position+operation.hop()<=pcm.size()){
        apply_at(plan,step,operation,fresh);
        const auto count=operation.hop();if(count>pcm.size()-position)break;
        const auto input=pcm.subspan(position,count);Row row,other;
        require(operation.process(input,row),"source or port complete-hop processing rejected");
        const bool equal=!fresh || (fresh->process(input,other) && identical(row,other));
        output.row(row,bool(fresh),equal,operation.memory());position+=count;++step;
    }
}
}
