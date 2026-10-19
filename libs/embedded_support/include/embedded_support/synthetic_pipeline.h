#pragma once
#include <embedded_support/profile.h>
#include <embedded_support/session.h>
#include <dataflow_runtime/runtime.h>
#include <array>
namespace embedded_support {
// Canonical default graph; an invalid profile produces an empty graph.
domain::NumericProcessingGraph default_processing_graph(const Profile&,std::string_view stream_id);
Status default_processing_graph(const Profile&,std::string_view stream_id,domain::NumericProcessingGraph&);
// Example composition of the sole public numeric executor. Cold prepare allocates;
// tick/capture callbacks are serialized and allocation-free. No capture fallback.
class SyntheticPipeline {
public:
    Status prepare(const Profile&,std::string_view stream_id);
    // Source-only preparation releases the standalone executor. Its Native PCM
    // callback is borrowed and may feed the host's sole GraphExecutor directly.
    Status prepare_source(const Profile&,std::string_view stream_id);
    Status tick_source(NumericFrameHandler,void*) noexcept;
    Status tick(NumericFrameHandler,void*) noexcept;
    Status process_pcm(const signal_contracts::DataFrameView&,NumericFrameHandler,void*) noexcept;
    signal_contracts::DataContract input_contract() const noexcept {return input_.view();}
    bdsp_protocol::numeric_data::Binding output_binding() const noexcept;
    application::MemoryRequirements memory_requirements() const noexcept;
private:
    void publish_source(const Profile&,std::string,std::vector<float>,domain::OwnedProcessingContract);
    void clear_processing() noexcept;
    void fill_source_samples() noexcept;
    signal_contracts::FrameStamp source_stamp() noexcept;
    Profile profile_;
    std::string id_;
    domain::OwnedProcessingContract input_,output_contract_;
    std::shared_ptr<const dataflow_runtime::PreparedGraph> plan_;
    std::unique_ptr<dataflow_runtime::RuntimeContext> context_;
    std::vector<float> pcm_;
    std::array<signal_contracts::PlaneLayout,2> planes_{};
    std::array<double,8> phase_{};
    std::uint64_t samples_=0,sequence_=0;
    domain::NodeId output_=1;
};
}
