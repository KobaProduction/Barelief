#pragma once
#include <bdsp_protocol/numeric_control_message.h>
#include <memory>

namespace control::numeric {
// Cold boundary only. No compatibility lowering or presentation fields.
bdsp_protocol::numeric_control::Result own_graph(
    const bdsp_protocol::numeric_control::Graph&,
    const bdsp_protocol::numeric_control::Limits&, domain::NumericProcessingGraph&);
bdsp_protocol::numeric_control::Result own_parameters(
    const bdsp_protocol::numeric_control::Parameters&,
    const bdsp_protocol::numeric_control::Limits&, domain::ProcessingParameters&);
// Cold preparation-adapter admission, BEFORE executor mutation. Measures a
// full applied graph + capabilities reply with every optional diagnostic and
// maximal scalar width in BOTH canonical codecs. Success reports componentwise
// maxima for wire/scratch/decode-arena/text; no buffer allocation or encoding.
bdsp_protocol::numeric_control::Result reply_envelope_fits(
    const domain::NumericProcessingGraph&,
    const bdsp_protocol::numeric_control::Limits&,
    const bdsp_protocol::numeric_control::Capabilities&);
// Heap-stable projection. Owns all metadata and parameter strings; wire() is
// borrowed until destruction/replacement, including after moving this owner.
class GraphProjection {
public:
    GraphProjection();
    ~GraphProjection();
    GraphProjection(GraphProjection&&) noexcept;
    GraphProjection& operator=(GraphProjection&&) noexcept;
    GraphProjection(const GraphProjection&)=delete;
    GraphProjection& operator=(const GraphProjection&)=delete;
    const bdsp_protocol::numeric_control::Graph& wire() const noexcept;
    bdsp_protocol::numeric_control::Result assign(const domain::NumericProcessingGraph&,
        const bdsp_protocol::numeric_control::Limits&);
private:
    struct Storage;
    std::unique_ptr<Storage> storage_;
};
}
