// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <pipeline/numeric_engine.h>
#include <QJsonObject>
#include <memory>

class ValueProbe {
public:
    ValueProbe(pipeline::NumericEngine&,const domain::NumericProcessingGraph&,const QString& endpoint);
    ~ValueProbe();
    bool satisfied() const;
    QJsonObject report() const;
private:
    struct State;
    pipeline::NumericEngine& engine_;
    std::shared_ptr<State> state_;
    std::uint64_t token_=0;
};
