// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dataflow_runtime/runtime.h>
#include <QDir>
#include <QJsonObject>
#include <memory>

// Cold-owned diagnostic storage. Observations never retain borrowed frame data.
class MadmomFrontendCapture {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    static constexpr std::size_t maximum_bytes=655360;
    explicit MadmomFrontendCapture(const dataflow_runtime::PreparedGraph&);
    ~MadmomFrontendCapture();
    void observe(const dataflow_runtime::NumericPortFrameView&) noexcept;
    bool complete() const noexcept;
    QJsonObject write(const QDir&) const;
};
