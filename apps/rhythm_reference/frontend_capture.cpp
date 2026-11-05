// SPDX-License-Identifier: AGPL-3.0-or-later
#include "frontend_capture.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonArray>
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace {
using namespace signal_contracts;
using namespace dataflow_runtime;
constexpr std::array<std::size_t,15> ordinals{0,1,2,92,93,94,95,96,97,192,193,194,195,196,197};
struct Specification {domain::NodeId node;domain::PortId port;std::size_t width;bool once=false;};
constexpr std::array<Specification,10> specifications{{
    {20,0,2048},{23,0,2048},{24,0,1024},{24,1,1023},{25,0,1024},
    {26,0,137},{29,0,137},{30,0,137},{3,0,1},{22,0,2048,true}}};
void require(bool value,const char* message) {if(!value)throw std::runtime_error(message);}
QString filename(Specification spec,DType dtype) {return QString("frontend-%1-%2.%3").arg(spec.node).arg(spec.port).arg(dtype==DType::Float64?"f64":"f32");}
QByteArray digest(const QByteArray& bytes) {return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex();}
void save(const QDir& directory,const QString& name,const QByteArray& bytes) {
    QFile file(directory.filePath(name));require(file.open(QIODevice::WriteOnly|QIODevice::NewOnly),"Cannot create diagnostic payload");
    require(file.write(bytes)==bytes.size() && file.flush(),"Cannot write diagnostic payload");
}
struct Trace {
    Specification spec;NumericContract contract;
    std::vector<std::byte> values;
    std::array<std::int64_t,ordinals.size()> timestamps{};
    std::array<std::size_t,ordinals.size()> captured_ordinals{};
    std::size_t seen=0,stored=0;unsigned error=0;
    explicit Trace(Specification requested,const PreparedGraph& plan):spec(requested) {
        const auto value=plan.port_contract(spec.node,domain::PortDirection::Output,spec.port);
        require(value && std::holds_alternative<NumericContract>(*value),"Missing frontend diagnostic port");
        contract=std::get<NumericContract>(*value);
        require((contract.dtype==DType::Float32 || ((spec.node==22 || spec.node==23) && contract.dtype==DType::Float64)) && contract.channel_order.size()==1,"Unexpected frontend diagnostic dtype");
        require(contract.shape.rank==(spec.width==1?Rank::Scalar:Rank::Vector) &&
            contract.shape.extents[0].maximum==spec.width,"Unexpected frontend diagnostic width");
        values.resize((spec.once?1:ordinals.size())*spec.width*element_bytes());
    }
    std::size_t element_bytes() const noexcept {return contract.dtype==DType::Float64?sizeof(double):sizeof(float);}
    bool copy_element(const NumericView& view,std::size_t source,std::size_t target) noexcept {
        std::uint64_t bits=0;if(load_bits(view,{0,source,0},contract.dtype,bits)!=Status::ok)return false;
        const double value=contract.dtype==DType::Float64?std::bit_cast<double>(bits):double(std::bit_cast<float>(std::uint32_t(bits)));
        if(!std::isfinite(value))return false;
        std::memcpy(values.data()+target*element_bytes(),&bits,element_bytes());return true;
    }
    bool copy(const NumericView& view,std::size_t row) noexcept {
        if(!view.contract || view.contract->dtype!=contract.dtype || view.planes.size()!=1 ||
            view.actual!=std::array<std::size_t,2>{spec.width,1})return false;
        for(std::size_t i=0;i<spec.width;++i)if(!copy_element(view,i,row*spec.width+i))return false;
        return true;
    }
    void observe(const NumericPortFrameView& event) noexcept {
        if(event.identity.node!=spec.node || event.identity.port!=spec.port)return;
        if(event.status==Status::unavailable && !event.frame.stamp.available && !seen)return;
        if(event.status!=Status::ok || !event.frame.stamp.available){error=1;return;}
        const auto ordinal=seen++;
        if(spec.once && stored)return;
        const auto selected=std::find(ordinals.begin(),ordinals.end(),ordinal);
        if(!spec.once && selected==ordinals.end())return;
        const auto row=spec.once?0:std::size_t(selected-ordinals.begin());
        capture(event,ordinal,row);
    }
    void capture(const NumericPortFrameView& event,std::size_t ordinal,std::size_t row) noexcept {
        const auto* value=std::get_if<NumericView>(&event.frame.payload);
        if(!value || !copy(*value,row)){error=2;return;}
        timestamps[row]=event.frame.stamp.timestamp_ticks;captured_ordinals[row]=ordinal;++stored;
    }
    bool complete() const noexcept {return !error && stored==(spec.once?1:ordinals.size()) && (spec.once || seen==198);}
    QJsonObject write(const QDir& directory) const {
        static_assert(std::endian::native==std::endian::little);
        const auto payload=QByteArray(reinterpret_cast<const char*>(values.data()),qsizetype(values.size()));
        const auto name=filename(spec,contract.dtype);save(directory,name,payload);QJsonArray frames;
        for(std::size_t i=0;i<(spec.once?1:ordinals.size());++i)
            frames.append(QJsonObject{{"ordinal",qint64(captured_ordinals[i])},{"timestamp_ticks",qint64(timestamps[i])}});
        return {{"node",spec.node},{"port",spec.port},{"file",name},{"dtype",contract.dtype==DType::Float64?"Float64":"Float32"},{"byte_order","little"},
            {"shape",QJsonArray{qint64(spec.once?1:ordinals.size()),qint64(spec.width)}},
            {"sha256",QString::fromLatin1(digest(payload))},{"observed_publications",qint64(seen)},
            {"captured_publications",qint64(stored)},{"error",int(error)},{"complete",complete()},
            {"clock_domain",QString::fromUtf8(contract.clock.domain.data(),qsizetype(contract.clock.domain.size()))},
            {"clock_rate",int(contract.clock.sample_rate)},{"tick_numerator",qint64(contract.clock.tick_seconds.numerator)},
            {"tick_denominator",qint64(contract.clock.tick_seconds.denominator)},{"frames",frames}};
    }
};
}
struct MadmomFrontendCapture::Impl {
    std::vector<Trace> traces;
    explicit Impl(const dataflow_runtime::PreparedGraph& plan) {
        traces.reserve(specifications.size());std::size_t bytes=0;
        for(const auto spec:specifications)bytes+=(spec.once?1:ordinals.size())*spec.width*((spec.node==22 || spec.node==23)?sizeof(double):sizeof(float));
        require(bytes<=maximum_bytes,"Frontend diagnostic exceeds cold storage budget");
        for(const auto spec:specifications)traces.emplace_back(spec,plan);
    }
};
MadmomFrontendCapture::MadmomFrontendCapture(const PreparedGraph& plan):impl_(std::make_unique<Impl>(plan)) {}
MadmomFrontendCapture::~MadmomFrontendCapture()=default;
void MadmomFrontendCapture::observe(const NumericPortFrameView& event) noexcept {
    if(event.direction!=domain::PortDirection::Output)return;
    for(auto& trace:impl_->traces)trace.observe(event);
}
bool MadmomFrontendCapture::complete() const noexcept {
    return std::all_of(impl_->traces.begin(),impl_->traces.end(),[](const Trace& trace){return trace.complete();});
}
QJsonObject MadmomFrontendCapture::write(const QDir& directory) const {
    QJsonArray ports;for(const auto& trace:impl_->traces)ports.append(trace.write(directory));
    return {{"complete",complete()},{"ports",ports},{"maximum_owned_bytes",qint64(maximum_bytes)}};
}
