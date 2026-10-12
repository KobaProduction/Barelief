#include <embedded_support/synthetic_pipeline.h>
#include <control/numeric_control_session.h>
#include <dataflow_runtime/runtime.h>
#include <cstdio>
struct Clock final:application::ControlClock {std::uint64_t now_ms() const noexcept override{return 1;}};
int main() {
    const embedded_support::Profile profile{};
    const auto effective=embedded_support::device_profile(profile);
    const auto admission=embedded_support::admit(profile,10);
    if(admission.status!=signal_contracts::Status::ok){std::fprintf(stderr,"SDK admission refused: status=%d frame=%zu wire=%zu required=%llu allowed=%llu reserved=%zu\n",int(admission.status),admission.frame_bytes,admission.wire_bytes,(unsigned long long)admission.required_bytes_per_second,(unsigned long long)admission.allowed_bytes_per_second,admission.reserved_bytes);return 1;}
    auto graph=embedded_support::default_processing_graph(profile,"sdk-source");
    dataflow_runtime::GraphExecutor executor(effective.runtime);
    application::PreparedExecution prepared;
    const application::NumericRuntimeLimits limits{effective.runtime.admission,effective.runtime.resources};
    if(executor.prepare_numeric(graph,limits,prepared)!=signal_contracts::Status::ok ||
       executor.apply(std::move(prepared))!=signal_contracts::Status::ok || !executor.running())return 2;
    embedded_support::SyntheticPipeline source;
    if(source.prepare_source(profile,"sdk-source")!=signal_contracts::Status::ok)return 3;
    struct Delivery {dataflow_runtime::GraphExecutor* executor;bool delivered=false;signal_contracts::Status status=signal_contracts::Status::ok;} delivery{&executor};
    source.tick_source([](void* opaque,const bdsp_protocol::numeric_data::Message& message) {
        auto& d=*static_cast<Delivery*>(opaque);
        d.status=d.executor->process_numeric_ports(0,message.frame_view(),{1,1},{nullptr,
            [](void* c,const dataflow_runtime::NumericPortFrameView& output) {
                auto& d=*static_cast<Delivery*>(c);if(output.kind!="NetworkOutput")return;
                const auto* value=std::get_if<signal_contracts::NumericView>(&output.frame.payload);
                d.delivered=value && value->contract && value->contract->semantic==signal_contracts::Semantic::PCM &&
                    value->contract->dtype==signal_contracts::DType::Float32 && value->contract->representation.encoding==signal_contracts::StorageEncoding::FixedLittleEndian &&
                    value->actual[0]==256 && value->planes.size()==1;
            },nullptr,&d});
    },&delivery);
    if(delivery.status!=signal_contracts::Status::ok || !delivery.delivered)return 4;
    Clock clock;control::numeric::SessionConfig config;config.endpoint_authorized=true;config.device_identity="sdk-consumer";config.client_identity=1;
    control::numeric::ControlSession control(config,clock);
    return control.open(1)==signal_contracts::Status::ok?0:5;
}
