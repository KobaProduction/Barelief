#include <core_app/numeric_view_adapter.h>
#include <cmath>
#include <algorithm>
namespace presentation {
using namespace signal_contracts;
namespace {
void caption(NumericCaptionPort* target,const char* source,QStringList args={},bool loss=false,bool unavailable=false) {
    if(target && target->caption)target->caption(source,std::move(args),loss,unavailable);
}
Status coordinates(const NumericView& view,size_t axis,size_t size,std::vector<double>& output) {
    const auto& declared=view.contract->coordinates[axis];
    if(declared.mode==CoordinateMode::None)return Status::ok;
    output.resize(size);
    if(declared.mode==CoordinateMode::Regular) {
        if(!declared.origin.denominator || !declared.step.denominator)return Status::malformed;
        for(size_t i=0;i<size;++i)output[i]=double(declared.origin.numerator)/double(declared.origin.denominator)+
            double(i)*double(declared.step.numerator)/double(declared.step.denominator);
    } else {
        const auto bytes=view.coordinates.axes[axis];if(bytes.size()!=size*8)return Status::malformed;
        for(size_t i=0;i<size;++i)output[i]=pipeline::decode_coordinate(bytes.subspan(i*8,8));
    }
    for(double value:output)if(!std::isfinite(value))return Status::malformed;
    return Status::ok;
}
}
Status NumericViewAdapter::bind(const pipeline::EndpointIdentity& endpoint,std::size_t plane) {
    Key key{endpoint,plane};if(states_.contains(key))return Status::ok;
    if(states_.size()>=limits_.max_bindings)return Status::exhausted;
    states_.try_emplace(std::move(key),limits_.history);return Status::ok;
}
void NumericViewAdapter::unbind(const pipeline::EndpointIdentity& endpoint,std::size_t plane){states_.erase(Key{endpoint,plane});}
void NumericViewAdapter::clear() noexcept {
    for(auto& [key,state]:states_){state.matrix_snapshot.clear();state.matrix_projected=false;state.latest={};state.history.clear();state.context.reset();state.status=Status::unavailable;state.history_mode=false;state.fence_seen=false;}
}
void NumericViewAdapter::clear_render_target(NumericRenderTarget target) {
    if(target.metrics)target.metrics->clear_values();
    if(target.chart){target.chart->update({});target.chart->set_scalar_readout({},false,true);caption(target.chart,"");}
    if(target.spectrogram){target.spectrogram->update_frame_history({});caption(target.spectrogram,"");}
    if(target.image){target.image->set_matrix({});caption(target.image,"");}
    if(target.pie){target.pie->set_values({});caption(target.pie,"");}
    if(!target.spectrogram || !target.spectrogram->paused())caption(target.exact_label,"");
}
Status NumericViewAdapter::accept(const pipeline::DeliveredNumericFrame& input) {
    if(!input.context || !input.context->contract)return Status::invalid_argument;
    Status result=Status::unavailable;
    for(auto& [key,state]:states_) {
        if(key.endpoint!=input.context->endpoint)continue;
        if(state.fence_seen && pipeline::delivery_precedes(input.delivery,state.delivery_fence))continue;
        state.delivery_fence=input.delivery;state.fence_seen=true;
        const bool changed=state.context && !same_contract(state.context->contract.view(),input.context->contract.view());
        if(changed || state.history_mode){state.history.clear();}
        state.context=input.context;
        state.matrix_snapshot.clear();state.matrix_projected=false;
        state.history_mode=false;state.status=input.status;
        if(!input.frame || input.status!=Status::ok){state.latest={};result=state.status;continue;}
        auto frame=input.frame.view();
        if(!frame.stamp.available){state.latest={};state.status=Status::unavailable;continue;}
        if(!changed && state.latest.frame && input.delivery.epoch==state.latest.delivery.epoch) {
            const auto prior=state.latest.frame.view().stamp;
            if(prior.producer_epoch==frame.stamp.producer_epoch && prior.generation==frame.stamp.generation && frame.stamp.sequence<=prior.sequence)continue;
        }
        if(const auto* numeric=std::get_if<NumericView>(&frame.payload)) {
            if(!numeric->contract || key.plane>=numeric->planes.size()){state.latest={};state.status=Status::invalid_argument;continue;}
            if(numeric->contract->shape.rank==Rank::Scalar) {
                NumericLiteral exact;state.status=pipeline::read_numeric_literal(*numeric,{key.plane,0,0},exact);
                if(state.status!=Status::ok){state.latest={};continue;}
                // Scalar graphs display the current supplied value. Use an
                // explicit History node to turn values into a time series.
            }
        }
        state.latest=input;result=state.status;
    }
    return result;
}
Status NumericViewAdapter::accept(const pipeline::DeliveredNumericHistoryRow& input) {
    if(!input.compact.context || !input.compact.context->contract)return Status::invalid_argument;
    Status result=Status::unavailable;
    for(auto& [key,state]:states_)if(key.endpoint==input.compact.context->endpoint) {
        if(state.fence_seen && pipeline::delivery_precedes(input.compact.delivery,state.delivery_fence))continue;
        state.delivery_fence=input.compact.delivery;state.fence_seen=true;
        // Compact rows are invalidation notices, never graph-owned accumulation.
        state.context=input.compact.context;result=Status::would_block;
    }
    return result;
}
Status NumericViewAdapter::accept_history_snapshot(const pipeline::DeliveredNumericFrame& input,std::uint64_t ordinal,std::uint64_t filled_count) {
    if(!input.context || !input.context->contract)return Status::invalid_argument;
    Status result=Status::unavailable;
    for(auto& [key,state]:states_)if(key.endpoint==input.context->endpoint) {
        if(state.fence_seen && pipeline::delivery_precedes(input.delivery,state.delivery_fence)){result=Status::would_block;continue;}
        const auto status=state.history.accept_snapshot(input,ordinal,filled_count,key.plane);
        if(status==Status::would_block){result=status;continue;}
        state.delivery_fence=input.delivery;state.fence_seen=true;
        state.matrix_snapshot.clear();state.matrix_projected=false;
        state.latest={};state.context=input.context;state.history_mode=true;state.status=status;result=status;
    }
    return result;
}
Status NumericViewAdapter::render(const pipeline::EndpointIdentity& endpoint,std::size_t plane,NumericRenderTarget target) {
    // Visibility controls only projection/drawing. accept() continues independently.
    if(!target.visible)return Status::ok;
    const auto found=states_.find(Key{endpoint,plane});if(found==states_.end()){
        if(target.metrics)target.metrics->set_unavailable(endpoint.port);
        return Status::unavailable;
    }
    auto& state=found->second;
    const auto failure=[&](Status status) {
        if(target.metrics)target.metrics->set_unavailable(endpoint.port);
        if(target.chart){target.chart->update({});target.chart->set_scalar_readout({},target.chart->scalar_readout_enabled(),false);caption(target.chart,"Renderer projection unavailable");}
        if(target.spectrogram){target.spectrogram->update_frame_history({});caption(target.spectrogram,"Renderer projection unavailable");}
        if(target.image){target.image->set_matrix({});caption(target.image,"Renderer projection unavailable");}
        if(target.pie){target.pie->set_values({});caption(target.pie,"Renderer projection unavailable");}
        if(!target.spectrogram || !target.spectrogram->paused())caption(target.exact_label,"Renderer projection unavailable");
        return status;
    };
    if(state.history_mode) {
        if(!state.history.columns())return failure(state.status);
        if(target.chart || target.pie || target.metrics)return failure(Status::incompatible_contract);
        if((target.spectrogram || target.image) && state.history.columns()) {
            // This renderer's geometry budget is separate from DSP admission.
            // Refuse outside its actual bounds without changing the running graph.
            if(state.history.capacity()>16384 || state.history.columns()>4096 ||
               state.history.capacity()>1048576/state.history.columns())return failure(Status::buffer_too_small);
            if(target.spectrogram)target.spectrogram->set_history_dimensions(state.history.capacity(),state.history.columns());
            const auto axes=state.history.render_axes();SpectrogramAxisMetadata metadata;
            metadata.time_title_key=axes.seconds?QStringLiteral("Time delay, seconds (X)"):QStringLiteral("Temporal frame (X)");
            metadata.time_coordinates=axes.seconds;metadata.ages=axes.ages;
            metadata.column_coordinates=true;metadata.columns=axes.columns;
            metadata.value_title_key=axes.value_axis==pipeline::HistoryValueAxis::Frames?QStringLiteral("Frames (Y)"):
                axes.value_axis==pipeline::HistoryValueAxis::Amplitude?QStringLiteral("Amplitude (Y)"):QStringLiteral("Value (Y)");
            switch(axes.column_axis) {
            case pipeline::HistoryColumnAxis::Band:metadata.column_title_key=QStringLiteral("Frequency, Mel bands (Z)");break;
            case pipeline::HistoryColumnAxis::Bin:metadata.column_title_key=QStringLiteral("Frequency bin (Z)");break;
            case pipeline::HistoryColumnAxis::Sample:metadata.column_title_key=QStringLiteral("Sample (Z)");break;
            case pipeline::HistoryColumnAxis::Value:metadata.column_title_key=QStringLiteral("Value index (Z)");break;
            case pipeline::HistoryColumnAxis::Hertz:metadata.column_title_key=QStringLiteral("Frequency, Hz (Z)");break;
            }
            const auto historyContract=std::get<NumericContract>(state.context->contract.view());
            metadata.column_index_title_key=historyContract.shape.axes[1]==Axis::Band?QStringLiteral("Frequency, Mel bands (Z)"):
                historyContract.shape.axes[1]==Axis::Bin?QStringLiteral("Frequency bin (Z)"):
                historyContract.shape.axes[1]==Axis::Sample?QStringLiteral("Sample (Z)"):QStringLiteral("Column index (Z)");
            if(!state.matrix_projected){
                state.matrix_snapshot=NumericMatrixSnapshot::take(state.history.render_snapshot());
                state.matrix_projected=true;
            }
            const auto& matrix=state.matrix_snapshot;
            if(target.spectrogram){
                if(!target.spectrogram->set_axis_metadata(metadata))return failure(Status::malformed);
                target.spectrogram->update_snapshot(matrix.transposed());
            }
            if(target.image){
                if(!target.image->set_axis_metadata(metadata))return failure(Status::malformed);
                target.image->set_snapshot(matrix,true);
            }
        }
        const QStringList args{QString::number(state.history.filled_count())};
        const bool unavailable=state.status!=Status::ok || !state.history.available();
        if(!target.spectrogram || !target.spectrogram->paused())caption(target.exact_label,"Frames: %1",args,state.history.precision_loss(),unavailable);
        caption(target.spectrogram,"Frames: %1",args,state.history.precision_loss(),unavailable);
        caption(target.image,"Frames: %1",args,state.history.precision_loss(),unavailable);
        caption(target.pie,"Frames: %1",args,state.history.precision_loss(),unavailable);
        return state.status;
    }
    if(!state.latest.frame || state.status!=Status::ok)return failure(state.status);
    const auto frame=state.latest.frame.view();
    // Packet presentation requires an explicit extractor, outside numeric graphs.
    if(!std::holds_alternative<NumericView>(frame.payload))return failure(Status::incompatible_contract);
    const auto& numeric=std::get<NumericView>(frame.payload);
    const auto rank=numeric.contract->shape.rank;
    if((target.chart || target.pie) && rank!=Rank::Vector)return failure(Status::incompatible_contract);
    if((target.spectrogram || target.image) && rank!=Rank::Matrix)return failure(Status::incompatible_contract);
    if(target.metrics) {
        if(rank!=Rank::Scalar)return failure(Status::incompatible_contract);
        NumericLiteral literal;const auto status=pipeline::read_numeric_literal(numeric,{plane,0,0},literal);
        if(status!=Status::ok)return failure(status);
        target.metrics->set_value(endpoint.port,QString::fromStdString(pipeline::exact_scalar_text(literal)));
        return Status::ok;
    }
    if(target.chart) {
        const bool scalar=numeric.contract->shape.rank==Rank::Scalar;
        const char* xTitle="Value index (X)";
        NumericLiteral current;
        if(scalar && pipeline::read_numeric_literal(numeric,{plane,0,0},current)!=Status::ok)return failure(Status::malformed);
        target.chart->set_scalar_readout(scalar?QString::fromStdString(pipeline::exact_scalar_text(current)):QString{},scalar,true);
        std::vector<double> coordinates;
        if(!scalar && numeric.contract->shape.rank==Rank::Vector) {
            switch(numeric.contract->shape.axes[0]) {
            case Axis::Band:xTitle="Band (X)";break;
            case Axis::Bin:xTitle="Frequency bin (X)";break;
            case Axis::Sample:xTitle="Sample (X)";break;
            default:break;
            }
            const auto& axis=numeric.contract->coordinates[0];
            if(axis.mode!=CoordinateMode::None && (axis.unit==NumericUnit::Hertz || axis.unit==NumericUnit::Seconds)) {
                if(numeric.actual[0]>limits_.vectors.max_elements)return failure(Status::buffer_too_small);
                coordinates.resize(numeric.actual[0]);
                if(axis.mode==CoordinateMode::Regular) {
                    if(!axis.origin.denominator || !axis.step.denominator)return failure(Status::malformed);
                    for(size_t i=0;i<coordinates.size();++i)
                        coordinates[i]=double(axis.origin.numerator)/double(axis.origin.denominator)+
                            double(i)*double(axis.step.numerator)/double(axis.step.denominator);
                } else {
                    const auto bytes=numeric.coordinates.axes[0];
                    if(bytes.size()!=coordinates.size()*8)return failure(Status::malformed);
                    for(size_t i=0;i<coordinates.size();++i)
                        coordinates[i]=pipeline::decode_coordinate(bytes.subspan(i*8,8));
                }
                xTitle=axis.unit==NumericUnit::Hertz?"Frequency, Hz (X)":"Time, seconds (X)";
            }
        }

        if(!target.chart->set_x_coordinates(coordinates))return failure(Status::malformed);
        const char* yTitle=numeric.contract->quantity==Quantity::FilledFrameCount || numeric.contract->unit==NumericUnit::Frames?"Frames (Y)":
            numeric.contract->quantity==Quantity::Amplitude || numeric.contract->unit==NumericUnit::NormalizedAmplitude || numeric.contract->unit==NumericUnit::RawAmplitude?"Amplitude (Y)":"Value (Y)";
        target.chart->set_axis_title_keys(QString::fromUtf8(xTitle),QString::fromUtf8(yTitle));
    }
    std::vector<float> projection;
    pipeline::FloatProjectionReport report;Status status;
    QString exact;
    if(rank==Rank::Matrix && state.matrix_projected) {
        status=Status::ok;report.inexact_elements=state.matrix_inexact_elements;
    } else if(numeric.contract->shape.rank==Rank::Scalar) {
        NumericLiteral current;status=pipeline::read_numeric_literal(numeric,{plane,0,0},current);
        if(status!=Status::ok)return failure(status);
        exact=QString::fromStdString(pipeline::exact_scalar_text(current));
        projection.resize(1);status=pipeline::project_numeric_float(numeric,plane,limits_.vectors,projection,report);
    } else {
        const auto columns=numeric.contract->shape.rank==Rank::Matrix?numeric.actual[1]:std::size_t{1};
        if(columns && numeric.actual[0]>limits_.vectors.max_elements/columns)return failure(Status::buffer_too_small);
        projection.resize(numeric.actual[0]*columns);
        status=pipeline::project_numeric_float(numeric,plane,limits_.vectors,projection,report);
    }
    if(status!=Status::ok) {
        failure(status);
        if(!exact.isEmpty()) {
            if(!target.spectrogram || !target.spectrogram->paused())caption(target.exact_label,"Exact value: %1",{exact},false,true);
            if(target.chart){target.chart->set_scalar_readout(exact,true,true);caption(target.chart,"Renderer projection unavailable",{},false,true);}
        }
        return status;
    }
    if(target.chart)target.chart->update(projection);
    if(target.pie){if(!target.pie->set_values(projection))return Status::malformed;caption(target.pie,"");}
    if(target.spectrogram || target.image) {
        const auto columns=numeric.contract->shape.rank==Rank::Matrix?numeric.actual[1]:projection.size();
        const auto rows=numeric.contract->shape.rank==Rank::Matrix?numeric.actual[0]:std::size_t{1};
        if(rows>16384 || columns>4096 || (columns && rows>1048576/columns))return failure(Status::buffer_too_small);
        if(!rows || !columns){if(target.spectrogram)target.spectrogram->update_frame_history({});if(target.image)target.image->set_matrix({});}
        else {
            if(!state.matrix_projected) {
                std::vector<std::vector<float>> matrix(rows,std::vector<float>(columns));
                for(size_t row=0;row<rows;++row)std::copy_n(projection.begin()+row*columns,columns,matrix[row].begin());
                state.matrix_snapshot=NumericMatrixSnapshot::take(std::move(matrix));
                state.matrix_inexact_elements=report.inexact_elements;state.matrix_projected=true;
            }
            const auto& matrix=state.matrix_snapshot;
            SpectrogramAxisMetadata metadata;metadata.time_title_key="Row index (X)";metadata.column_title_key="Column index (Z)";
            metadata.time_index_title_key="Row index (X)";
            metadata.age_axis=false;
            const bool matrix_rank=numeric.contract->shape.rank==Rank::Matrix;
            const size_t column_axis=matrix_rank?1:0;
            if(numeric.contract->shape.rank!=Rank::Scalar){
                if(coordinates(numeric,column_axis,columns,metadata.columns)!=Status::ok)return failure(Status::malformed);
                metadata.column_coordinates=!metadata.columns.empty();
                if(numeric.contract->coordinates[column_axis].unit==NumericUnit::Hertz && metadata.column_coordinates)
                    metadata.column_title_key="Frequency, Hz (Z)";
                else if(numeric.contract->shape.axes[column_axis]==Axis::Band)metadata.column_title_key="Band (Z)";
                else if(numeric.contract->shape.axes[column_axis]==Axis::Bin)metadata.column_title_key="Frequency bin (Z)";
                else if(numeric.contract->shape.axes[column_axis]==Axis::Sample)metadata.column_title_key="Sample (Z)";
                metadata.column_index_title_key=numeric.contract->shape.axes[column_axis]==Axis::Band?QStringLiteral("Frequency, Mel bands (Z)"):
                    numeric.contract->shape.axes[column_axis]==Axis::Bin?QStringLiteral("Frequency bin (Z)"):
                    numeric.contract->shape.axes[column_axis]==Axis::Sample?QStringLiteral("Sample (Z)"):QStringLiteral("Column index (Z)");
            }
            if(matrix_rank){
                if(coordinates(numeric,0,rows,metadata.ages)!=Status::ok)return failure(Status::malformed);
                metadata.time_coordinates=!metadata.ages.empty();
                if(numeric.contract->coordinates[0].unit==NumericUnit::Seconds && metadata.time_coordinates)
                    metadata.time_title_key="Time, seconds (X)";
            }
            // Surface geometry addresses rows by reverse index. Map labels back
            // to the supplied matrix without rearranging its values.
            if(target.spectrogram){
                auto surface_metadata=metadata;surface_metadata.time_coordinates=true;
                surface_metadata.ages.clear();
                for(size_t row=0;row<rows;++row){
                    surface_metadata.ages.push_back(metadata.time_coordinates?metadata.ages[rows-1-row]:double(rows-1-row));
                    surface_metadata.row_indices.push_back(double(rows-1-row));
                }
                target.spectrogram->set_history_dimensions(rows,columns);
                if(!target.spectrogram->set_axis_metadata(surface_metadata))return failure(Status::malformed);
                target.spectrogram->update_snapshot(matrix.transposed());
            }
            if(target.image){
                if(!target.image->set_axis_metadata(metadata))return failure(Status::malformed);
                target.image->set_snapshot(matrix,false);
            }
        }
    }
    const bool loss=report.inexact_elements!=0;
    const auto source=exact.isEmpty()?"":"Exact value: %1";
    const auto args=exact.isEmpty()?QStringList{}:QStringList{exact};
    if(!target.spectrogram || !target.spectrogram->paused())caption(target.exact_label,source,args,loss);
    if(target.chart)caption(target.chart,numeric.contract->shape.rank==Rank::Scalar?"":source,
        numeric.contract->shape.rank==Rank::Scalar?QStringList{}:args,loss);
    return Status::ok;
}
}
