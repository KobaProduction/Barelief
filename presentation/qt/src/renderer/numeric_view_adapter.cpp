#include <core_app/numeric_view_adapter.h>
#include <barelief/render_preparation_policy.h>
#include <barelief/numeric_plot_limits.h>
#include <cmath>
#include <algorithm>
namespace presentation {
using namespace signal_contracts;
namespace {
bool is_vector_history(const NumericContract& contract) {
    return contract.semantic==Semantic::History && contract.shape.rank==Rank::Vector;
}
bool is_vector_history(const pipeline::DeliveredNumericFrame& input) {
    const auto contract=input.context->contract.view();
    const auto* numeric=std::get_if<NumericContract>(&contract);
    return numeric && is_vector_history(*numeric);
}
Status remember_vector_history_fill(const pipeline::DeliveredNumericFrame& input,std::uint64_t filled_count,
    Status status,std::optional<std::size_t>& occupancy) {
    if(status!=Status::ok || !input.frame)return status;
    // The processing node owns the ring; the GUI remembers only occupancy.
    const auto snapshot=input.frame.view();
    if(const auto* view=std::get_if<NumericView>(&snapshot.payload))
        occupancy=std::size_t(std::min<std::uint64_t>(filled_count,view->actual[0]));
    return status;
}
void caption(NumericCaptionPort* target,const char* source,QStringList args={},bool loss=false,bool unavailable=false) {
    if(target && target->caption)target->caption(source,std::move(args),loss,unavailable);
}
void clear_matrix_targets(NumericRenderTarget target,const char* message) {
    if(target.spectrogram){target.spectrogram->update_frame_history({});caption(target.spectrogram,message);}
    if(target.image){target.image->set_matrix({});caption(target.image,message);}
    if(target.pie){target.pie->set_values({});caption(target.pie,message);}
}
Status regular_coordinates(const AxisCoordinates& declared,size_t size,std::vector<double>& output) {
    if(!declared.origin.denominator || !declared.step.denominator)return Status::malformed;
    for(size_t i=0;i<size;++i)output[i]=double(declared.origin.numerator)/double(declared.origin.denominator)+
        double(i)*double(declared.step.numerator)/double(declared.step.denominator);
    return Status::ok;
}
Status explicit_coordinates(const NumericView& view,size_t axis,size_t size,std::vector<double>& output) {
    const auto bytes=view.coordinates.axes[axis];
    if(bytes.size()!=size*8)return Status::malformed;
    // HistoryWriter already exports an oldest-to-newest coordinate span,
    // even when numeric samples use a wrapped ring. Do not rotate twice.
    for(size_t i=0;i<size;++i)
        output[i]=pipeline::decode_coordinate(bytes.subspan(i*8,8));
    return Status::ok;
}
Status coordinates(const NumericView& view,size_t axis,size_t size,std::vector<double>& output) {
    const auto& declared=view.contract->coordinates[axis];
    if(declared.mode==CoordinateMode::None)return Status::ok;
    output.resize(size);
    const auto status=declared.mode==CoordinateMode::Regular?
        regular_coordinates(declared,size,output):explicit_coordinates(view,axis,size,output);
    if(status!=Status::ok)return status;
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
    for(auto& [key,state]:states_){state.matrix_snapshot.clear();state.matrix_pool.clear();state.matrix_projected=false;state.latest={};state.history.clear();state.context.reset();state.status=Status::unavailable;state.history_mode=false;state.vector_history_filled.reset();state.fence_seen=false;}
}
void NumericViewAdapter::clear_render_target(NumericRenderTarget target) {
    if(target.metrics)target.metrics->clear_values();
    if(target.chart){target.chart->update({});target.chart->set_scalar_readout({},false,true);caption(target.chart,"");}
    clear_matrix_targets(target,"");
    if(!target.spectrogram || !target.spectrogram->paused())caption(target.exact_label,"");
}
Status NumericViewAdapter::accept(const pipeline::DeliveredNumericFrame& input) {
    if(!input.context || !input.context->contract)return Status::invalid_argument;
    Status result=Status::unavailable;
    for(auto& [key,state]:states_) {
        if(key.endpoint!=input.context->endpoint)continue;
        if(const auto status=accept_frame_for_state(key,state,input))result=*status;
    }
    return result;
}
std::optional<Status> NumericViewAdapter::accept_frame_for_state(const Key& key,State& state,
    const pipeline::DeliveredNumericFrame& input) {
    if(state.fence_seen && pipeline::delivery_precedes(input.delivery,state.delivery_fence))return std::nullopt;
    const bool changed=reset_frame_state(state,input);
    if(!input.frame || input.status!=Status::ok){state.latest={};return state.status;}
    return accept_available_frame(key,state,input,changed);
}
std::optional<Status> NumericViewAdapter::accept_available_frame(const Key& key,State& state,
    const pipeline::DeliveredNumericFrame& input,bool changed) {
    const auto frame=input.frame.view();
    if(!frame.stamp.available){state.latest={};state.status=Status::unavailable;return std::nullopt;}
    if(stale_frame_sequence(state,input,changed))return std::nullopt;
    if(const auto* numeric=std::get_if<NumericView>(&frame.payload))
        if(validate_scalar_frame(key,state,*numeric)!=Status::ok)return std::nullopt;
    state.latest=input;
    return state.status;
}
bool NumericViewAdapter::reset_frame_state(State& state,const pipeline::DeliveredNumericFrame& input) {
    state.delivery_fence=input.delivery;state.fence_seen=true;
    const bool changed=state.context && !same_contract(state.context->contract.view(),input.context->contract.view());
    if(changed || state.history_mode)state.history.clear();
    state.context=input.context;
    state.matrix_snapshot.clear();state.matrix_projected=false;
    state.history_mode=false;state.vector_history_filled.reset();state.status=input.status;
    return changed;
}
bool NumericViewAdapter::stale_frame_sequence(const State& state,const pipeline::DeliveredNumericFrame& input,bool changed) const {
    if(changed || !state.latest.frame || input.delivery.epoch!=state.latest.delivery.epoch)return false;
    const auto prior=state.latest.frame.view().stamp;
    const auto current=input.frame.view().stamp;
    return prior.producer_epoch==current.producer_epoch && prior.generation==current.generation && current.sequence<=prior.sequence;
}
Status NumericViewAdapter::validate_scalar_frame(const Key& key,State& state,const NumericView& numeric) {
    if(!numeric.contract || key.plane>=numeric.planes.size()){
        state.latest={};state.status=Status::invalid_argument;return state.status;
    }
    if(numeric.contract->shape.rank==Rank::Scalar) {
        NumericLiteral exact;
        state.status=pipeline::read_numeric_literal(numeric,{key.plane,0,0},exact);
        if(state.status!=Status::ok)state.latest={};
    }
    // Scalar graphs display the supplied value; History nodes own time series.
    return state.status;
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
    const bool vector_history=is_vector_history(input);
    Status result=Status::unavailable;
    for(auto& [key,state]:states_)if(key.endpoint==input.context->endpoint) {
        if(!vector_history) {
            result=accept_snapshot_for_state(key,state,input,ordinal,filled_count);
            continue;
        }
        if(const auto status=accept_frame_for_state(key,state,input))
            result=remember_vector_history_fill(input,filled_count,*status,state.vector_history_filled);
    }
    return result;
}
Status NumericViewAdapter::accept_snapshot_for_state(const Key& key,State& state,
    const pipeline::DeliveredNumericFrame& input,std::uint64_t ordinal,std::uint64_t filled_count) {
    if(state.fence_seen && pipeline::delivery_precedes(input.delivery,state.delivery_fence))return Status::would_block;
    const auto status=state.history.accept_snapshot(input,ordinal,filled_count,key.plane);
    if(status==Status::would_block)return status;
    state.delivery_fence=input.delivery;state.fence_seen=true;
    state.matrix_snapshot.clear();state.matrix_projected=false;
    state.latest={};state.context=input.context;state.history_mode=true;state.status=status;
    return status;
}
static Status projection_failure(NumericRenderTarget target,domain::PortId port,Status status) {
        if(target.metrics)target.metrics->set_unavailable(port);
        if(target.chart){target.chart->update({});target.chart->set_scalar_readout({},target.chart->scalar_readout_enabled(),false);caption(target.chart,"Renderer projection unavailable");}
        clear_matrix_targets(target,"Renderer projection unavailable");
        if(target.piano){target.piano->set_values({});caption(target.piano,"Renderer projection unavailable");}
        if(!target.spectrogram || !target.spectrogram->paused())caption(target.exact_label,"Renderer projection unavailable");
        return status;
}
Status NumericViewAdapter::render(const pipeline::EndpointIdentity& endpoint,std::size_t plane,NumericRenderTarget target) {
    if(!target.visible)return Status::ok;
    const auto found=states_.find(Key{endpoint,plane});
    if(found==states_.end())return projection_failure(target,endpoint.port,Status::unavailable);
    auto& state=found->second;
    return state.history_mode?render_history(state,target):render_frame(state,endpoint,plane,target);
}
namespace {
Status publish_snapshot(const NumericMatrixSnapshot& matrix,const SpectrogramAxisMetadata& metadata,NumericRenderTarget target,bool history){
    if(target.spectrogram){
        if(!target.spectrogram->set_axis_metadata(metadata))return Status::malformed;
        target.spectrogram->update_snapshot(matrix.transposed());
    }
    if(target.image){
        if(!target.image->set_axis_metadata(metadata))return Status::malformed;
        target.image->set_snapshot(matrix,history);
    }
    return Status::ok;
}
}
SpectrogramAxisMetadata NumericViewAdapter::history_axes(const State& state,NumericRenderTarget target)const {
    const auto axes=state.history.render_axes();SpectrogramAxisMetadata metadata;
    metadata.time_title_key=axes.seconds && target.axes.time?target.presentation.row_title:target.presentation.row_index_title;
    metadata.time_index_title_key=target.presentation.row_index_title;
    metadata.time_coordinates=axes.seconds && target.axes.time;metadata.ages=axes.ages;
    metadata.column_coordinates=target.axes.frequency && axes.column_coordinates;metadata.columns=axes.columns;
    metadata.value_title_key=target.presentation.value_title;metadata.column_title_key=target.presentation.column_title;
    metadata.column_index_title_key=target.presentation.column_index_title;metadata.value_scale=target.presentation.value_scale;
    normalize_history_axes(metadata,target);
    return metadata;
}
void NumericViewAdapter::normalize_history_axes(SpectrogramAxisMetadata& metadata,NumericRenderTarget target) const {
    if(!metadata.time_coordinates)for(size_t i=0;i<metadata.ages.size();++i)metadata.ages[i]=double(i);
    if(!metadata.column_coordinates){metadata.column_title_key=metadata.column_index_title_key;for(size_t i=0;i<metadata.columns.size();++i)metadata.columns[i]=double(i);}
}
void NumericViewAdapter::prepare_history_snapshot(State& state){
    if(state.matrix_projected)return;
    state.matrix_snapshot=RenderPreparationPolicy::snapshot_pool?state.matrix_pool.fill([&](auto& rows){state.history.render_snapshot_into(rows);}):NumericMatrixSnapshot::take(state.history.render_snapshot());
    state.matrix_projected=true;
}
void NumericViewAdapter::history_captions(const State& state,NumericRenderTarget target)const {
    const QStringList args{QString::number(state.history.filled_count())};
    const bool unavailable=state.status!=Status::ok || !state.history.available();
    if(!target.spectrogram || !target.spectrogram->paused())caption(target.exact_label,"Frames: %1",args,state.history.precision_loss(),unavailable);
    caption(target.spectrogram,"Frames: %1",args,state.history.precision_loss(),unavailable);
    caption(target.image,"Frames: %1",args,state.history.precision_loss(),unavailable);
    caption(target.pie,"Frames: %1",args,state.history.precision_loss(),unavailable);
}
Status NumericViewAdapter::render_history(State& state,NumericRenderTarget target){
    if(!state.history.columns())return projection_failure(target,0,state.status);
    if(target.chart || target.pie || target.metrics)return projection_failure(target,0,Status::incompatible_contract);
    if(target.spectrogram || target.image){
        const auto status=publish_history_snapshot(state,target);
        if(status!=Status::ok)return projection_failure(target,0,status);
    }
    history_captions(state,target);return state.status;
}
Status NumericViewAdapter::publish_history_snapshot(State& state,NumericRenderTarget target) {
    if(!NumericPlotLimits::accepts(state.history.capacity(),state.history.columns()))return Status::buffer_too_small;
    if(target.spectrogram)target.spectrogram->set_history_dimensions(state.history.capacity(),state.history.columns());
    prepare_history_snapshot(state);
    return publish_snapshot(state.matrix_snapshot,history_axes(state,target),target,true);
}
namespace {
Status fill_frequency_coordinates(std::size_t count,NumericRenderTarget::FrequencyGrid grid,std::vector<double>& output){
    output.resize(count);
    for(std::size_t i=0;i<output.size();++i){
        output[i]=(double(grid.first_bin)+double(i))*grid.step;
        if(!std::isfinite(output[i]))return Status::malformed;
    }
    return Status::ok;
}
Status explicit_vector_coordinates(const NumericView& numeric,NumericRenderTarget target,std::size_t maximum,std::vector<double>& output,QString& title){
    title=target.frequency_grid.index_title;const auto step=target.frequency_grid.step;
    if(!std::isfinite(step) || step<0)return Status::malformed;
    if(!step)return Status::ok;
    if(numeric.actual[0]>maximum)return Status::buffer_too_small;
    const auto status=fill_frequency_coordinates(numeric.actual[0],target.frequency_grid,output);
    if(status!=Status::ok)return status;
    title=target.frequency_grid.coordinate_title;return Status::ok;
}
Status scalar_readout(const NumericView& numeric,std::size_t plane,NumericRenderTarget target){
    const bool scalar=numeric.contract->shape.rank==Rank::Scalar;NumericLiteral current;
    if(scalar && pipeline::read_numeric_literal(numeric,{plane,0,0},current)!=Status::ok)return Status::malformed;
    target.chart->set_scalar_readout(scalar?QString::fromStdString(pipeline::exact_scalar_text(current)):QString{},scalar,true);return Status::ok;
}
Status history_vector_coordinates(const NumericView& numeric,const NumericRenderTarget& target,
    std::size_t filled,std::vector<double>& values,QString& title) {
    if(filled>numeric.actual[0])return Status::malformed;
    if(!target.axes.time || numeric.contract->coordinates[0].mode==CoordinateMode::None)return Status::ok;
    const auto status=coordinates(numeric,0,numeric.actual[0],values);
    if(status!=Status::ok)return status;
    values.erase(values.begin(),values.end()-std::ptrdiff_t(filled));
    if(!values.empty())title="Time, seconds (X)";
    return Status::ok;
}
}
Status NumericViewAdapter::configure_vector(const NumericView& numeric,std::size_t plane,
                                           NumericRenderTarget target,std::optional<std::size_t> filled){
    auto status=scalar_readout(numeric,plane,target);if(status!=Status::ok)return status;
    QString title=target.presentation.vector_x_title;std::vector<double> values;
    status=filled && is_vector_history(*numeric.contract)
        ?history_vector_coordinates(numeric,target,*filled,values,title)
        :vector_coordinates(numeric,target,values,title);
    if(status!=Status::ok)return status;
    if(!target.chart->set_x_coordinates(values))return Status::malformed;
    if(target.chart->set_value_source)target.chart->set_value_source(target.presentation.value_scale.source_decibels,target.presentation.value_scale.factor);
    target.chart->set_axis_title_keys(title,target.presentation.value_title);return Status::ok;
}
Status NumericViewAdapter::vector_coordinates(const NumericView& numeric,NumericRenderTarget target,
    std::vector<double>& values,QString& title) {
    if(numeric.contract->shape.rank==Rank::Scalar)return Status::ok;
    if(target.frequency_grid.explicit_input)
        return explicit_vector_coordinates(numeric,target,limits_.vectors.max_elements,values,title);
    const bool timed_history=target.axes.time && numeric.contract &&
        numeric.contract->semantic==Semantic::History &&
        numeric.contract->coordinates[0].unit==NumericUnit::Seconds &&
        numeric.contract->coordinates[0].mode!=CoordinateMode::None;
    if(target.axes.frequency || timed_history) {
        const auto status=coordinates(numeric,0,numeric.actual[0],values);
        if(status==Status::ok && timed_history && !values.empty())
            title="Time, seconds (X)";
        return status;
    }
    return Status::ok;
}
void NumericViewAdapter::prepare_matrix_snapshot(State& state,std::size_t rows,std::size_t columns,const std::vector<float>& projection,const pipeline::FloatProjectionReport& report){
    if(state.matrix_projected)return;
    const auto fill=[&](auto& matrix){matrix.resize(rows);for(size_t row=0;row<rows;++row){matrix[row].resize(columns);std::copy_n(projection.begin()+row*columns,columns,matrix[row].begin());}};
    if(RenderPreparationPolicy::snapshot_pool)state.matrix_snapshot=state.matrix_pool.fill(fill);
    else {NumericMatrixSnapshot::Matrix matrix;fill(matrix);state.matrix_snapshot=NumericMatrixSnapshot::take(std::move(matrix));}
    state.matrix_inexact_elements=report.inexact_elements;state.matrix_projected=true;
}
namespace {
Status matrix_axes(const NumericView& numeric,std::size_t rows,std::size_t columns,NumericRenderTarget target,SpectrogramAxisMetadata& metadata){
    metadata.time_index_title_key=target.presentation.row_index_title;metadata.column_index_title_key=target.presentation.column_index_title;metadata.age_axis=false;
    metadata.value_title_key=target.presentation.value_title;metadata.value_scale=target.presentation.value_scale;
    if(target.axes.frequency && coordinates(numeric,1,columns,metadata.columns)!=Status::ok)return Status::malformed;
    if(target.axes.time && coordinates(numeric,0,rows,metadata.ages)!=Status::ok)return Status::malformed;
    metadata.column_coordinates=!metadata.columns.empty();metadata.time_coordinates=!metadata.ages.empty();
    metadata.time_title_key=metadata.time_coordinates?target.presentation.row_title:target.presentation.row_index_title;
    metadata.column_title_key=metadata.column_coordinates?target.presentation.column_title:target.presentation.column_index_title;return Status::ok;
}
SpectrogramAxisMetadata surface_axes(SpectrogramAxisMetadata metadata,std::size_t rows){
    const auto times=metadata.ages;metadata.ages.clear();
    for(size_t row=0;row<rows;++row){metadata.ages.push_back(metadata.time_coordinates?times[rows-1-row]:double(rows-1-row));metadata.row_indices.push_back(double(rows-1-row));}
    return metadata;
}
Status publish_matrix(const NumericMatrixSnapshot& matrix,const SpectrogramAxisMetadata& metadata,std::size_t rows,std::size_t columns,NumericRenderTarget target){
    if(target.spectrogram){
        target.spectrogram->set_history_dimensions(rows,columns);
        if(!target.spectrogram->set_axis_metadata(surface_axes(metadata,rows)))return Status::malformed;
        target.spectrogram->update_snapshot(matrix.transposed());
    }
    target.spectrogram=nullptr;return publish_snapshot(matrix,metadata,target,false);
}
}
Status NumericViewAdapter::render_matrix(State& state,const NumericView& numeric,const std::vector<float>& projection,const pipeline::FloatProjectionReport& report,NumericRenderTarget target){
    const auto columns=numeric.actual[1],rows=numeric.actual[0];
    if(!NumericPlotLimits::accepts(rows,columns))return projection_failure(target,0,Status::buffer_too_small);
    if(!rows || !columns){
        if(target.spectrogram)target.spectrogram->update_frame_history({});
        if(target.image)target.image->set_matrix({});
        return Status::ok;
    }
    prepare_matrix_snapshot(state,rows,columns,projection,report);
    SpectrogramAxisMetadata metadata;const auto status=matrix_axes(numeric,rows,columns,target,metadata);
    if(status!=Status::ok)return projection_failure(target,0,status);
    return publish_matrix(state.matrix_snapshot,metadata,rows,columns,target);
}
namespace {
bool target_accepts_rank(NumericRenderTarget target,Rank rank){
    if(target.chart && rank!=Rank::Vector && rank!=Rank::Scalar)return false;
    if((target.pie || target.piano) && rank!=Rank::Vector)return false;
    if((target.spectrogram || target.image) && rank!=Rank::Matrix)return false;
    return !target.metrics || rank==Rank::Scalar;
}
Status render_metric(const NumericView& numeric,std::size_t plane,std::uint16_t port,NumericRenderTarget target){
    NumericLiteral value;const auto status=pipeline::read_numeric_literal(numeric,{plane,0,0},value);
    if(status==Status::ok)target.metrics->set_value(port,QString::fromStdString(pipeline::exact_scalar_text(value)));
    return status;
}
Status publish_piano(const NumericView& numeric,const std::vector<float>& values,NumericRenderTarget target){
    std::vector<double> hz;
    if(target.axes.frequency && target.presentation.piano_frequency_coordinates && coordinates(numeric,0,values.size(),hz)!=Status::ok)return Status::malformed;
    if(!target.piano->set_frequencies(hz) || !target.piano->set_values(values))return Status::malformed;
    caption(target.piano,"");return Status::ok;
}
Status publish_radial_views(const NumericView& numeric,const std::vector<float>& values,NumericRenderTarget target) {
    if(target.pie){
        if(!target.pie->set_values(values))return Status::malformed;
        caption(target.pie,"");
    }
    if(target.piano)return publish_piano(numeric,values,target);
    return Status::ok;
}
void frame_captions(NumericRenderTarget target,bool scalar,const QString& exact,bool loss){
    const auto source=exact.isEmpty()?"":"Exact value: %1";const auto args=exact.isEmpty()?QStringList{}:QStringList{exact};
    if(!target.spectrogram || !target.spectrogram->paused())caption(target.exact_label,source,args,loss);
    if(target.chart)caption(target.chart,scalar?"":source,scalar?QStringList{}:args,loss);
    if(target.piano)caption(target.piano,source,args,loss);
}
Status frame_projection_failure(NumericRenderTarget target,std::uint16_t port,Status status,const QString& exact){
    projection_failure(target,port,status);
    if(exact.isEmpty())return status;
    if(!target.spectrogram || !target.spectrogram->paused())caption(target.exact_label,"Exact value: %1",{exact},false,true);
    if(target.chart){target.chart->set_scalar_readout(exact,true,true);caption(target.chart,"Renderer projection unavailable",{},false,true);}
    return status;
}
Status prepare_scalar_projection(const NumericView& numeric,std::size_t plane,
    std::vector<float>& values,QString& exact) {
    NumericLiteral value;
    const auto status=pipeline::read_numeric_literal(numeric,{plane,0,0},value);
    if(status!=Status::ok)return status;
    exact=QString::fromStdString(pipeline::exact_scalar_text(value));values.resize(1);
    return Status::ok;
}
Status resize_projection(const NumericView& numeric,std::size_t plane,std::size_t maximum,
    std::vector<float>& values,QString& exact) {
    const auto rank=numeric.contract->shape.rank;
    if(rank==Rank::Scalar)return prepare_scalar_projection(numeric,plane,values,exact);
    const auto columns=rank==Rank::Matrix?numeric.actual[1]:std::size_t{1};
    if(columns && numeric.actual[0]>maximum/columns)return Status::buffer_too_small;
    values.resize(numeric.actual[0]*columns);return Status::ok;
}
Status trim_history_projection(const NumericView& numeric,std::optional<std::size_t> filled,std::vector<float>& values) {
    if(!filled || !is_vector_history(*numeric.contract))return Status::ok;
    if(*filled>values.size())return Status::malformed;
    values.erase(values.begin(),values.end()-std::ptrdiff_t(*filled));
    return Status::ok;
}
}
Status NumericViewAdapter::project_frame(State& state,const NumericView& numeric,std::size_t plane,FrameProjection& projection){
    const auto rank=numeric.contract->shape.rank;
    if(rank==Rank::Matrix && state.matrix_projected){projection.report.inexact_elements=state.matrix_inexact_elements;return Status::ok;}
    if(const auto status=resize_projection(numeric,plane,limits_.vectors.max_elements,projection.values,projection.exact);status!=Status::ok)return status;
    const auto status=pipeline::project_numeric_float(numeric,plane,limits_.vectors,projection.values,projection.report);
    if(status!=Status::ok)return status;
    return trim_history_projection(numeric,state.vector_history_filled,projection.values);
}
Status NumericViewAdapter::publish_frame(State& state,const NumericView& numeric,FrameProjection& projection,NumericRenderTarget target){
    if(target.chart)target.chart->update(projection.values);
    if(const auto status=publish_radial_views(numeric,projection.values,target);status!=Status::ok)return status;
    if(target.spectrogram || target.image){
        const auto status=render_matrix(state,numeric,projection.values,projection.report,target);
        if(status!=Status::ok)return status;
    }
    frame_captions(target,numeric.contract->shape.rank==Rank::Scalar,projection.exact,projection.report.inexact_elements!=0);return Status::ok;
}
Status NumericViewAdapter::render_frame(State& state,const pipeline::EndpointIdentity& endpoint,std::size_t plane,NumericRenderTarget target){
    if(!state.latest.frame || state.status!=Status::ok)return projection_failure(target,endpoint.port,state.status);
    const auto frame=state.latest.frame.view();
    if(!std::holds_alternative<NumericView>(frame.payload))return projection_failure(target,endpoint.port,Status::incompatible_contract);
    const auto& numeric=std::get<NumericView>(frame.payload);
    return render_numeric_frame(state,endpoint,plane,numeric,target);
}
Status NumericViewAdapter::render_numeric_frame(State& state,const pipeline::EndpointIdentity& endpoint,
    std::size_t plane,const NumericView& numeric,NumericRenderTarget target) {
    if(!target_accepts_rank(target,numeric.contract->shape.rank))return projection_failure(target,endpoint.port,Status::incompatible_contract);
    if(target.metrics){const auto status=render_metric(numeric,plane,endpoint.port,target);return status==Status::ok?status:projection_failure(target,endpoint.port,status);}
    if(target.chart){const auto status=configure_vector(numeric,plane,target,state.vector_history_filled);if(status!=Status::ok)return projection_failure(target,endpoint.port,status);}
    FrameProjection projection;const auto status=project_frame(state,numeric,plane,projection);
    if(status!=Status::ok)return frame_projection_failure(target,endpoint.port,status,projection.exact);
    const auto published=publish_frame(state,numeric,projection,target);
    return published==Status::ok?published:projection_failure(target,endpoint.port,published);
}
}
