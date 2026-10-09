#include <esp_idf_adapter/nvs_persistence.h>
#include <embedded_support/persistence.h>
#include <embedded_support/synthetic_pipeline.h>
#include <array>
#include <vector>
#include <cstring>
#include <cstdio>
namespace {
std::array<std::vector<std::uint8_t>,2> durable,pending;
int target=-1,writes=0,commits=0,failures=0;bool interrupt=false,no_space=false;
int slot(const char* k){return !std::strcmp(k,"processing_a")?0:!std::strcmp(k,"processing_b")?1:-1;}
#define CHECK(x) do{if(!(x)){++failures;std::printf("FAIL %d: %s\n",__LINE__,#x);}}while(false)
}
esp_err_t nvs_get_blob(nvs_handle_t h,const char* key,void* out,std::size_t* size) {
    CHECK(h==42);auto i=slot(key);if(i<0 || durable[i].empty())return ESP_ERR_NVS_NOT_FOUND;
    if(!out){*size=durable[i].size();return ESP_OK;}
    if(*size<durable[i].size()){*size=durable[i].size();return ESP_ERR_NVS_INVALID_LENGTH;}
    std::memcpy(out,durable[i].data(),durable[i].size());*size=durable[i].size();return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t h,const char* key,const void* in,std::size_t size) {
    CHECK(h==42);++writes;if(no_space)return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    target=slot(key);CHECK(target>=0);auto* b=static_cast<const std::uint8_t*>(in);pending[target].assign(b,b+size);return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t h) {
    CHECK(h==42);++commits;CHECK(target>=0);durable[target]=pending[target];
    if(interrupt){durable[target].resize(durable[target].size()/2);return ESP_FAIL;}return ESP_OK;
}
namespace {
struct Clock final:application::ControlClock {std::uint64_t now_ms() const noexcept override{return 10;}};
void applied_save_reboot() {
    using signal_contracts::Status;namespace es=embedded_support;
    durable={};pending={};interrupt=no_space=false;target=-1;writes=commits=0;
    esp_idf_adapter::NvsProcessingStorage nvs(42,16432);
    std::array<std::uint8_t,16432> first{},second{},scratch{};
    alignas(std::max_align_t) std::array<std::byte,16384> arena{},staging{};
    std::array<std::size_t,16> workspace{};
    es::Profile p;auto device=es::device_profile(p);auto caps=es::numeric_capabilities(device);
    bdsp_protocol::numeric_control::CborCodec codec;
    es::ProcessingPersistence store(nvs,{first,second,scratch,arena,staging,workspace},codec,
        es::numeric_control_limits(p),device.runtime.admission,caps);
    es::AppliedProcessingPersistence persistence(store);dataflow_runtime::GraphExecutor executor(device.runtime);
    application::NumericRuntimeLimits limits{device.runtime.admission,device.runtime.resources};
    application::GraphLifecycle lifecycle(executor,executor,caps,limits);application::GraphLifecycleControlPort port(lifecycle);
    Clock clock;application::ProcessingCapabilities capabilities;capabilities.control.max_sessions=1;
    capabilities.explicit_persist=true;capabilities.numeric=application::NumericProcessingCapabilities{caps,limits};
    std::array<application::ControlSessionSlot,1> sessions;
    application::ProcessingControlService service(port,clock,capabilities,sessions,123,&persistence);
    const auto session=service.open_session(42);CHECK(session);if(!session)return;
    application::ProcessingDTO dto;dto.numeric=es::default_processing_graph(p,"source");
    auto prepared=service.submit(session.identity,{1,0},application::PrepareProcessing{&dto});CHECK(prepared.accepted && writes==0);
    if(!prepared.accepted)return;
    auto applied=service.submit(session.identity,{2,0},application::ApplyProcessing{prepared.prepared_token,"embedded-synthetic"});
    CHECK(applied.accepted && writes==0 && commits==0);if(!applied.accepted)return;
    auto saved=service.submit(session.identity,{3,applied.state.revision},application::ExplicitPersist{applied.state.graph_revision});
    CHECK(saved.accepted && writes==1 && commits==1);
    auto replay=service.submit(session.identity,{3,applied.state.revision},application::ExplicitPersist{applied.state.graph_revision});
    CHECK(replay.accepted && writes==1 && commits==1);
    interrupt=true;
    auto interrupted=service.submit(session.identity,{4,saved.state.revision},application::ExplicitPersist{applied.state.graph_revision});
    CHECK(!interrupted.accepted && executor.running() && writes==2 && commits==2);
    esp_idf_adapter::NvsProcessingStorage reboot_nvs(42,16432);
    es::ProcessingPersistence reboot(reboot_nvs,{first,second,scratch,arena,staging,workspace},codec,
        es::numeric_control_limits(p),device.runtime.admission,caps);
    domain::NumericProcessingGraph restored;CHECK(reboot.restore(restored)==Status::ok);
    CHECK(application::same_numeric_processing_graph(restored,*dto.numeric) && writes==2 && commits==2);
}
}
int main() {
    using signal_contracts::Status;
    esp_idf_adapter::NvsProcessingStorage storage(42,16);
    std::array<std::uint8_t,16> out{};std::size_t n=123;
    CHECK(storage.read(0,out,n)==Status::unavailable && n==123 && writes==0 && commits==0);
    const std::array<std::uint8_t,4> old{1,2,3,4},next{5,6,7,8};
    CHECK(storage.commit(0,old)==Status::ok);CHECK(storage.read(0,out,n)==Status::ok && n==4 && out[3]==4);
    interrupt=true;CHECK(storage.commit(1,next)==Status::disconnected);
    CHECK(storage.read(0,out,n)==Status::ok && n==4 && out[3]==4); // last slot untouched
    CHECK(storage.read(1,out,n)==Status::ok && n==2); // interrupted candidate visible for CRC refusal
    no_space=true;auto before=commits;CHECK(storage.commit(1,next)==Status::exhausted && commits==before);
    n=123;CHECK(storage.read(0,std::span(out).first(1),n)==Status::buffer_too_small && n==123);
    durable[1].resize(17);CHECK(storage.read(1,out,n)==Status::buffer_too_small && n==123);
    before=writes;CHECK(storage.commit(2,next)==Status::invalid_argument && writes==before);
    CHECK(storage.read(2,out,n)==Status::invalid_argument);CHECK(storage.commit(0,{})==Status::invalid_argument);
    applied_save_reboot();
    std::printf("embedded fake NVS adapter: %d failures\n",failures);return failures?1:0;
}
