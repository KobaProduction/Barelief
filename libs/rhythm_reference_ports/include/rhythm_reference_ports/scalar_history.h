// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <span>
#include <vector>
namespace rhythm_reference_ports {
// Prepared bounded scalar history. A mirrored ring exposes one contiguous
// oldest-first view without copying the full window on the processing path.
class ScalarHistory {
public:
    bool prepare(std::size_t count);
    // Mirrored ring payload only; excludes this object and caller stack.
    static bool storage_bytes(std::size_t count,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return values_.capacity()*sizeof(float);}
    void set(std::size_t index,float value) noexcept;
    void push(float value) noexcept;
    std::span<const float> values() const noexcept {return std::span(values_).subspan(head_,count_);}
    void clear() noexcept;
    // Rebind the active length within the prepared capacity and zero it.
    // Invalidates borrowed values, but does not allocate or copy a history.
    bool reset_length(std::size_t count,float initial_value=0) noexcept;
private:
    std::size_t count_=0,head_=0;
    std::vector<float> values_;
};
}
