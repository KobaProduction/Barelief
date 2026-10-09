#pragma once
#include <signal_contracts/frame.h>
#include <domain/processing_contracts.h>
#include <bdsp_protocol/numeric_control_codec.h>
#include <application/processing_control.h>

namespace embedded_support {
// Serialized cold path. Two separately committed blobs; no erase/repair on read.
// Failed commit may damage ONLY the addressed slot, never the other slot.
class ProcessingStoragePort {
public:
    virtual ~ProcessingStoragePort() = default;
    virtual signal_contracts::Status read(unsigned slot, std::span<std::uint8_t>, std::size_t& bytes) noexcept = 0;
    virtual signal_contracts::Status commit(unsigned slot, std::span<const std::uint8_t>) noexcept = 0;
};
struct PersistenceBuffers {
    std::span<std::uint8_t> first, second, encode_scratch;
    std::span<std::byte> decode_arena, decode_staging;
    std::span<std::size_t> admission_workspace;
};
// Existing graph codec + domain admission, no JSON/UI/session/token persistence.
// All disjoint caller buffers/capability context/storage outlive this object.
// Save is the sole write; restore publishes an owning graph only after admission.
class ProcessingPersistence {
public:
    static constexpr std::size_t record_header_bytes = 48;
    ProcessingPersistence(ProcessingStoragePort&, PersistenceBuffers,
        const bdsp_protocol::numeric_control::Codec&, bdsp_protocol::numeric_control::Limits,
        domain::ProcessingAdmissionLimits, domain::NumericDeviceCapabilities) noexcept;
    signal_contracts::Status save(const domain::NumericProcessingGraph&);
    signal_contracts::Status restore(domain::NumericProcessingGraph&);
private:
    struct Record { signal_contracts::Status status; std::uint64_t sequence=0; std::span<const std::uint8_t> payload; };
    Record read(unsigned slot, std::span<std::uint8_t>) noexcept;
    signal_contracts::Status decode(const Record&, domain::NumericProcessingGraph&);
    ProcessingStoragePort& storage_;
    PersistenceBuffers buffers_;
    const bdsp_protocol::numeric_control::Codec& codec_;
    bdsp_protocol::numeric_control::Limits codec_limits_;
    domain::ProcessingAdmissionLimits admission_;
    domain::NumericDeviceCapabilities capabilities_;
};
// Connects explicit Persist on the sole application service to processing-only
// storage. Revision remains application state and is never serialized to flash.
class AppliedProcessingPersistence final : public application::ProcessingPersistencePort {
public:
    explicit AppliedProcessingPersistence(ProcessingPersistence& store) noexcept :store_(store) {}
    signal_contracts::Status persist_applied(const application::ProcessingDTO& dto,std::uint64_t) override {
        return dto.numeric?store_.save(*dto.numeric):signal_contracts::Status::unsupported_version;
    }
private:
    ProcessingPersistence& store_;
};
}
