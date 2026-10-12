#pragma once
#include <signal_contracts/data_frame.h>
namespace dataflow_runtime::detail {
using signal_contracts::Ratio;
bool spectral_vector(const signal_contracts::NumericContract&) noexcept;
bool latency_greater(Ratio,Ratio) noexcept;
bool divide_products(std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t&,std::uint64_t* fraction=nullptr) noexcept;
bool convert_rate_ticks(std::int64_t,std::uint32_t,std::uint32_t,std::int64_t&) noexcept;
bool same_fraction(Ratio,Ratio) noexcept;
}
