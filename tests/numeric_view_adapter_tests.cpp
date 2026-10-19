#include "numeric_render_fixture.h"
#include <core_app/numeric_view_adapter.h>
#include <visualization/charts/line.h>
#include <visualization/charts/freq.h>
#include <visualization/ui_text.h>
#include <visualization/numeric_image_view.h>
#include <visualization/numeric_pie_view.h>
#include <visualization/numeric_piano_view.h>
#include <QDir>
#include <QFontDatabase>
#include <visualization/numeric_metrics_view.h>
#include <visualization/spectrograms/spectrogram_graph.h>
#include <visualization/render_preparation_policy.h>
#include <QApplication>
#include <QLabel>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QThread>
#include <array>
#include <dataflow_runtime/runtime.h>
#include <cstdio>
using namespace signal_contracts;
namespace {
int failures=0;
#define CHECK(condition) do { if(!(condition)){std::printf("FAIL line %d: %s\n",__LINE__,#condition);++failures;} } while(false)
constexpr FrameLimits limits{2,16,4096,4096,128,4096};
const std::array<std::uint32_t,1> channels{0};
struct Storage {
    alignas(64) std::array<std::byte,16384> payload{},metadata{};
    std::array<BufferSlot,32> payload_slots{},metadata_slots{};
    BufferPool p{payload,payload_slots,512},m{metadata,metadata_slots,512};
};
auto context(const NumericContract& contract,int node) {
    auto result=std::make_shared<pipeline::NumericDeliveryContext>();
    result->endpoint={"display-regression",node,domain::PortDirection::Input,0};
    CHECK(domain::own_processing_contract(contract,limits,4096,result->contract)==Status::ok);
    return result;
}
NumericContract scalar_contract() {
    NumericContract contract;contract.dtype=DType::UInt64;contract.channel_order=channels;
    return contract;
}
pipeline::DeliveredNumericFrame scalar(Storage& storage,
    std::shared_ptr<const pipeline::NumericDeliveryContext> ctx,const NumericContract& contract,
    std::uint64_t sequence,std::uint64_t value,pipeline::DeliveryStamp delivery={1,1},std::int64_t ticks=0) {
    std::array<PlaneLayout,1> planes{{{0,{8,8}}}};
    NumericView view{&contract,std::as_bytes(std::span(&value,1)),planes,{1,1}};
    FrameStamp stamp;stamp.sequence=sequence;stamp.producer_epoch=9;stamp.timestamp_ticks=ticks;
    pipeline::DeliveredNumericFrame result;
    CHECK(pipeline::retain_numeric_delivery(std::move(ctx),delivery,{stamp,view},Status::ok,limits,storage.p,storage.m,result)==Status::ok);
    return result;
}
void hidden_scalar_and_contract_transition() {
    Storage storage;const auto contract=scalar_contract();auto ctx=context(contract,1);
    auto mutableOther=context(contract,2);mutableOther->endpoint.port=9;
    std::shared_ptr<const pipeline::NumericDeliveryContext> other=mutableOther;
    presentation::NumericViewAdapter adapter({4,{3,2,56,{true,2}},{true,128}});
    CHECK(adapter.bind(ctx->endpoint)==Status::ok);CHECK(adapter.bind(other->endpoint)==Status::ok);
    NumericMetricsView metrics;metrics.set_inputs({{0,"First"},{9,"Second"}});
    CHECK(metrics.input_count()==2);
    render_fixture::Target target{nullptr,nullptr,nullptr,false,nullptr,nullptr,&metrics};
    for(std::uint64_t i=0;i<130;++i) {
        CHECK(adapter.accept(scalar(storage,ctx,contract,i,i))==Status::ok);
        CHECK(adapter.accept(scalar(storage,other,contract,i,1000+i))==Status::ok);
        CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok);
    }
    CHECK(!metrics.value_available(0));target.visible=true;
    CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok);CHECK(metrics.exact_value(0)=="129");
    CHECK(adapter.render(other->endpoint,0,target)==Status::ok);CHECK(metrics.exact_value(9)=="1129" && metrics.exact_value(0)=="129");
    pipeline::DeliveredNumericFrame unavailable{ctx,{1,1},{},Status::unavailable};
    CHECK(adapter.accept(unavailable)==Status::unavailable);
    CHECK(adapter.render(ctx->endpoint,0,target)==Status::unavailable);
    CHECK(!metrics.value_available(0) && metrics.exact_value(9)=="1129");
    CHECK(adapter.accept(scalar(storage,ctx,contract,130,130))==Status::ok);
    CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok);CHECK(metrics.exact_value(0)=="130");
    auto count_contract=contract;count_contract.quantity=Quantity::FilledFrameCount;count_contract.unit=NumericUnit::Frames;
    auto count_context=context(count_contract,1);
    CHECK(adapter.accept(scalar(storage,count_context,count_contract,131,3,{2,1}))==Status::ok);
    CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok);CHECK(metrics.exact_value(0)=="3");
    CHECK(adapter.accept(scalar(storage,count_context,count_contract,132,4,{3,1}))==Status::ok);
    unavailable.context=count_context;unavailable.delivery={2,1};adapter.accept(unavailable);
    CHECK(adapter.render(count_context->endpoint,0,target)==Status::ok);CHECK(metrics.exact_value(0)=="4");
    adapter.clear();CHECK(adapter.render(count_context->endpoint,0,target)==Status::unavailable);
    CHECK(!metrics.value_available(0));
    CHECK(adapter.accept(scalar(storage,count_context,count_contract,0,7,{1,0}))==Status::ok);
    CHECK(adapter.render(count_context->endpoint,0,target)==Status::ok);CHECK(metrics.exact_value(0)=="7");
    for(const auto* language:{"ru","en"}){ui::Language::instance().set(language);CHECK(metrics.exact_value(0)=="7");}
    metrics.set_inputs({{9,"Renamed"}});CHECK(metrics.input_count()==1 && metrics.input_name(0).isEmpty());
    CHECK(adapter.render(other->endpoint,0,target)==Status::unavailable);
}
void hidden_history_and_markers() {
    Storage storage;NumericContract history;history.dtype=DType::UInt64;history.semantic=Semantic::History;
    history.history_element=Semantic::Numeric;history.channel_order=channels;
    history.shape={Rank::Matrix,{Axis::Time,Axis::Declared},{ExtentBounds{0,3},ExtentBounds{2,2}}};
    auto ctx=context(history,3);auto row=history;row.semantic=Semantic::Numeric;
    row.shape={Rank::Vector,{Axis::Declared,Axis::None},{ExtentBounds{2,2},ExtentBounds{}}};
    presentation::NumericViewAdapter adapter({1,{3,2,56,{true,2}},{true,128}});adapter.bind(ctx->endpoint);
    QLabel label;label.setText("hidden history");render_fixture::Target target{nullptr,nullptr,&label,false};
    const auto snapshot=[&](std::uint64_t ordinal) {
        std::array<std::uint64_t,6> values{};const auto filled=std::min(ordinal,std::uint64_t{3});
        for(std::size_t i=0;i<filled;++i){values[(3-filled+i)*2]=ordinal-filled+i+1;values[(3-filled+i)*2+1]=ordinal-filled+i+2;}
        const std::array<PlaneLayout,1> planes{{{0,{16,8}}}};
        FrameStamp stamp;stamp.sequence=ordinal;stamp.producer_epoch=8;
        pipeline::DeliveredNumericFrame result{ctx,{3,5},{},Status::ok};
        CHECK(retain_frame({stamp,NumericView{&history,std::as_bytes(std::span(values)),planes,{3,2},{3,0}}},limits,storage.p,storage.m,result.frame)==Status::ok);
        return result;
    };
    for(std::uint64_t ordinal=1;ordinal<=5;++ordinal) {
        CHECK(adapter.accept_history_snapshot(snapshot(ordinal),ordinal,std::min(ordinal,std::uint64_t{3}))==Status::ok);
        CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok);
    }
    CHECK(label.text()=="hidden history");target.visible=true;
    CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok);CHECK(label.text().contains("3"));
    for(const auto kind:{pipeline::NumericHistoryEvent::Empty,pipeline::NumericHistoryEvent::Reset,pipeline::NumericHistoryEvent::Unavailable}) {
        pipeline::DeliveredNumericHistoryRow stale;stale.compact.context=ctx;stale.compact.delivery={2,5};stale.capacity=3;stale.event=kind;
        adapter.accept(stale);CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok);CHECK(label.text().contains("3"));
    }
    pipeline::DeliveredNumericHistoryRow invalidation;invalidation.compact.context=ctx;invalidation.compact.delivery={3,5};invalidation.capacity=3;
    CHECK(adapter.accept(invalidation)==Status::would_block);
    CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok);CHECK(label.text().contains("3"));
    // Invalid full snapshots visibly flag availability while preserving known rows.
    CHECK(adapter.accept_history_snapshot(snapshot(6),6,4)==Status::malformed);
    CHECK(adapter.render(ctx->endpoint,0,target)==Status::malformed);
    CHECK(label.property("numericCaptionUnavailable").toBool());
    // Missing authoritative payload clears both the previous geometry and its
    // occupied-count caption, rather than preserving stale History rows.
    SpectrogramGraph surface;target=render_fixture::Target{nullptr,&surface,&label,target.visible};
    CHECK(adapter.render(ctx->endpoint,0,target)==Status::malformed);
    CHECK(surface.projected_history_frames()==3);
    pipeline::DeliveredNumericFrame unavailable{ctx,{3,5},{},Status::unavailable};
    CHECK(adapter.accept_history_snapshot(unavailable,6,3)==Status::unavailable);
    CHECK(adapter.render(ctx->endpoint,0,target)==Status::unavailable);
    CHECK(label.text()==ui::text("Renderer projection unavailable"));
    CHECK(label.property("numericCaptionArguments").toStringList().isEmpty());
    CHECK(surface.projected_history_frames()==0);
    CHECK(surface.projected_status()==ui::text("Renderer projection unavailable"));
    float staleValue=0;CHECK(!surface.projected_history_sample(0,0,staleValue));
    presentation::NumericViewAdapter::clear_render_target(target);CHECK(label.text().isEmpty());
    ui::Language::instance().set("ru");CHECK(label.text().isEmpty());
    ui::Language::instance().set("en");CHECK(label.text().isEmpty());
}
void scalar_timed_readout() {
    Storage storage;auto contract=scalar_contract();contract.clock={"scalar-source",{1,1000},1000};
    auto ctx=context(contract,20);presentation::NumericViewAdapter adapter({1,{3,2,56,{true,2}},{true,128}});
    CHECK(adapter.bind(ctx->endpoint)==Status::ok);
    NumericMetricsView metrics;metrics.set_inputs({{0,"Exact metric"}});

    render_fixture::Target target{nullptr,nullptr,nullptr,false,nullptr,nullptr,&metrics};
    for(std::uint64_t i=0;i<140;++i) {
        CHECK(adapter.accept(scalar(storage,ctx,contract,i,9007199254740993ULL+i,{1,1},std::int64_t(i*i)))==Status::ok);
        CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok);
    }
    CHECK(!metrics.value_available(0));target.visible=true;
    CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok);CHECK(metrics.exact_value(0)=="9007199254741132");
    CHECK(metrics.input_count()==1 && metrics.metrics_model()->rowCount()==1); // exact current value, no graph-owned history
    LineChartView chart;SpectrogramGraph surface;NumericImageView image;NumericPieView pie;
    CHECK(adapter.render(ctx->endpoint,0,render_fixture::Target{&chart,nullptr,nullptr,true})==Status::ok);
    CHECK(chart.scalar_readout_enabled() && chart.scalar_readout_text()=="9007199254741132");
    CHECK(adapter.render(ctx->endpoint,0,render_fixture::Target{nullptr,&surface,nullptr,true})==Status::incompatible_contract);
    CHECK(adapter.render(ctx->endpoint,0,render_fixture::Target{nullptr,nullptr,nullptr,true,&image})==Status::incompatible_contract);
    CHECK(adapter.render(ctx->endpoint,0,render_fixture::Target{nullptr,nullptr,nullptr,true,nullptr,&pie})==Status::incompatible_contract);
    CHECK(adapter.accept(scalar(storage,ctx,contract,141,std::numeric_limits<std::uint64_t>::max()))==Status::ok);
    CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok);CHECK(metrics.exact_value(0)=="18446744073709551615");
    pipeline::DeliveredNumericFrame unavailable{ctx,{1,1},{},Status::unavailable};
    CHECK(adapter.accept(unavailable)==Status::unavailable);CHECK(adapter.render(ctx->endpoint,0,target)==Status::unavailable);
    CHECK(!metrics.value_available(0));
    CHECK(adapter.accept(scalar(storage,ctx,contract,142,40,{1,1},400))==Status::ok);
    CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok && metrics.exact_value(0)=="40");
}
void universal_numeric_ranks() {
    Storage storage;presentation::NumericViewAdapter adapter({4,{4,4,256,{true,16}},{true,16}});
    LineChartView chart;SpectrogramGraph surface;NumericImageView image;NumericPieView pie;QLabel exact;
    NumericContract contract;contract.channel_order=channels;contract.semantic=Semantic::History; // Passive tag, not a ring.
    const auto deliver=[&](Rank rank,int node,std::size_t rows,std::size_t columns) {
        contract.shape={rank,{Axis::Declared,rank==Rank::Matrix?Axis::Declared:Axis::None},
            {ExtentBounds{rows,rows},rank==Rank::Matrix?ExtentBounds{columns,columns}:ExtentBounds{}}};
        if(rank==Rank::Scalar)contract.shape={};
        else if(rank==Rank::Vector)contract.shape.extents[0]={columns,columns};
        auto ctx=context(contract,node);CHECK(adapter.bind(ctx->endpoint)==Status::ok);
        std::vector<float> values(rows*columns);for(size_t i=0;i<values.size();++i)values[i]=float(i+1);
        const std::array<PlaneLayout,1> planes{{{0,{rank==Rank::Matrix?columns*4:4,4}}}};
        NumericView view{&contract,std::as_bytes(std::span(values)),planes,{rows,columns}};
        if(rank!=Rank::Matrix){view.actual={rank==Rank::Scalar?1:columns,1};}
        pipeline::DeliveredNumericFrame frame;FrameStamp stamp;stamp.sequence=1;
        CHECK(pipeline::retain_numeric_delivery(ctx,{1,1},{stamp,view},Status::ok,limits,storage.p,storage.m,frame)==Status::ok);
        CHECK(adapter.accept(frame)==Status::ok);
        const auto vectorStatus=rank==Rank::Vector?Status::ok:Status::incompatible_contract;
        const auto matrixStatus=rank==Rank::Matrix?Status::ok:Status::incompatible_contract;
        CHECK(adapter.render(ctx->endpoint,0,render_fixture::Target{&chart,nullptr,&exact,true})==(rank==Rank::Matrix?Status::incompatible_contract:Status::ok));
        if(rank==Rank::Scalar)CHECK(chart.scalar_readout_enabled() && chart.scalar_readout_text()=="1");
        CHECK(adapter.render(ctx->endpoint,0,render_fixture::Target{nullptr,nullptr,nullptr,true,nullptr,&pie})==vectorStatus);
        CHECK(adapter.render(ctx->endpoint,0,render_fixture::Target{nullptr,&surface,nullptr,true,&image})==matrixStatus);
        if(rank==Rank::Vector){CHECK(chart.value_count()==values.size());CHECK(pie.slice_count()==values.size());}
        if(rank!=Rank::Matrix)return;
        CHECK(surface.projection_storage_identity()!=nullptr);
        CHECK(surface.projection_storage_identity()==image.projection_storage_identity());
        const auto* shared_storage=surface.projection_storage_identity();
        SpectrogramGraph second_surface;
        CHECK(adapter.render(ctx->endpoint,0,render_fixture::Target{nullptr,&second_surface,nullptr,true})==Status::ok);
        CHECK(second_surface.projection_storage_identity()==shared_storage);
        CHECK(adapter.render(ctx->endpoint,0,render_fixture::Target{nullptr,&surface,nullptr,true,&image})==Status::ok);
        CHECK(surface.projection_storage_identity()==shared_storage && image.projection_storage_identity()==shared_storage);
        CHECK(image.matrix_dimensions()==QSize(int(columns),int(rows)));
        CHECK(surface.projected_history_frames()==rows && surface.projected_history_bands()==columns);
        float projected=0;
        for(size_t row=0;row<rows;++row)for(size_t column=0;column<columns;++column) {
            CHECK(surface.projected_history_sample(row,column,projected) && projected==values[row*columns+column]);
            CHECK(image.matrix_sample(row,column,projected) && projected==values[row*columns+column]);
        }
        CHECK(!surface.projected_axis_metadata().age_axis);
        CHECK(surface.projected_axis_metadata().age(0)==double(rows-1));
        CHECK(surface.projected_axis_metadata().age(rows-1)==0);
        CHECK(image.row_coordinate(0)==0 && image.row_coordinate(rows-1)==double(rows-1));
    };
    deliver(Rank::Scalar,31,1,1);deliver(Rank::Vector,32,1,3);deliver(Rank::Matrix,33,2,3);
    contract.semantic=Semantic::PCM;contract.shape={Rank::Matrix,{Axis::Declared,Axis::Declared},{ExtentBounds{2,2},ExtentBounds{3,3}}};
    contract.coordinates[0]={CoordinateMode::Regular,NumericUnit::Seconds,{5,1},{2,1}};
    auto ctx=context(contract,34);CHECK(adapter.bind(ctx->endpoint)==Status::ok);
    const std::array<float,6> values{1,2,3,4,5,6};const std::array<PlaneLayout,1> planes{{{0,{12,4}}}};
    pipeline::DeliveredNumericFrame frame;CHECK(pipeline::retain_numeric_delivery(ctx,{1,1},{{},NumericView{&contract,std::as_bytes(std::span(values)),planes,{2,3}}},Status::ok,limits,storage.p,storage.m,frame)==Status::ok);
    CHECK(adapter.accept(frame)==Status::ok && adapter.render(ctx->endpoint,0,render_fixture::Target{nullptr,&surface,nullptr,true,&image})==Status::ok);
    CHECK(surface.projected_axis_metadata().age(0)==7 && surface.projected_axis_metadata().age(1)==5);
    CHECK(image.row_coordinate(0)==5 && image.row_coordinate(1)==7);
}
void temporal_matrix_axes() {
    Storage storage;presentation::NumericViewAdapter adapter({1,{4,4,256,{true,16}},{true,16}});
    SpectrogramGraph surface;NumericImageView image;
    NumericContract contract;contract.channel_order=channels;contract.semantic=Semantic::SpectrumMagnitude;
    contract.shape={Rank::Matrix,{Axis::Band,Axis::Bin},{ExtentBounds{2,2},ExtentBounds{3,3}}};
    const std::array<float,6> values{1,2,3,11,12,13};const std::array<PlaneLayout,1> planes{{{0,{12,4}}}};
    render_fixture::Target target{nullptr,&surface,nullptr,true,&image};
    target.plot_presentation.row_title="Band (X)";target.plot_presentation.row_index_title="Band (X)";
    target.plot_presentation.column_title="Modulation frequency bin (Z)";target.plot_presentation.column_index_title="Modulation frequency bin (Z)";
    const auto deliver=[&](std::uint64_t sequence,std::span<const std::byte> band_coordinates={},std::span<const std::byte> bin_coordinates={}) {
        auto ctx=context(contract,35);CHECK(adapter.bind(ctx->endpoint)==Status::ok);
        NumericView numeric{&contract,std::as_bytes(std::span(values)),planes,{2,3}};
        numeric.coordinates.axes[0]=band_coordinates;numeric.coordinates.axes[1]=bin_coordinates;
        FrameStamp stamp;stamp.sequence=sequence;pipeline::DeliveredNumericFrame event;
        CHECK(pipeline::retain_numeric_delivery(ctx,{1,1},{stamp,numeric},Status::ok,limits,storage.p,storage.m,event)==Status::ok);
        CHECK(adapter.accept(event)==Status::ok);CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok);
        return ctx;
    };
    deliver(1);
    CHECK(surface.projected_axis_metadata().time_title_key=="Band (X)");
    CHECK(surface.projected_axis_metadata().column_title_key=="Modulation frequency bin (Z)");
    CHECK(!surface.projected_axis_metadata().age_axis);
    float value=0;
    CHECK(surface.projected_history_sample(0,2,value) && value==3);
    CHECK(surface.projected_history_sample(1,0,value) && value==11);
    CHECK(image.matrix_sample(1,2,value) && value==13);
    CHECK(image.row_coordinate(0)==0 && image.row_coordinate(1)==1);
    contract.coordinates[0]={CoordinateMode::Regular,NumericUnit::Hertz,{70,1},{25,1}};
    contract.coordinates[1]={CoordinateMode::Regular,NumericUnit::Hertz,{0,1},{120,512}};
    target.plot_presentation.row_title="Source frequency, Hz (X)";target.plot_presentation.column_title="Modulation frequency, Hz (Z)";
    deliver(2);
    CHECK(surface.projected_axis_metadata().time_title_key=="Source frequency, Hz (X)");
    CHECK(surface.projected_axis_metadata().column_title_key=="Modulation frequency, Hz (Z)");
    CHECK(surface.projected_axis_metadata().age(0)==95 && surface.projected_axis_metadata().age(1)==70);
    CHECK(image.row_coordinate(0)==70 && image.row_coordinate(1)==95);
    CHECK(surface.projected_axis_metadata().column(2)==240./512.);
    const auto positions=surface.projected_axis_metadata().displayed("positions","positions");
    CHECK(positions.time_title_key=="Band (X)" && positions.column_title_key=="Modulation frequency bin (Z)");
    target.axes={false,false};deliver(3);
    CHECK(surface.projected_axis_metadata().time_title_key=="Band (X)" && surface.projected_axis_metadata().column_title_key=="Modulation frequency bin (Z)");
    CHECK(!surface.projected_axis_metadata().time_coordinates && !surface.projected_axis_metadata().column_coordinates);
    target.axes={true,true};
    contract.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Hertz};
    contract.coordinates[1]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Hertz};
    std::array<std::byte,16> band_coordinates{};std::array<std::byte,24> bin_coordinates{};
    const auto encode=[](std::span<std::byte> bytes,std::size_t index,double value){const auto bits=std::bit_cast<std::uint64_t>(value);for(unsigned byte=0;byte<8;++byte)bytes[index*8+byte]=std::byte((bits>>(byte*8))&255);};
    encode(band_coordinates,0,70);encode(band_coordinates,1,95);for(std::size_t bin=0;bin<3;++bin)encode(bin_coordinates,bin,double(bin)*.25);
    const auto ctx=deliver(4,band_coordinates,bin_coordinates);
    CHECK(surface.projected_axis_metadata().column(2)==.5 && image.row_coordinate(1)==95);
    pipeline::DeliveredNumericFrame unavailable{ctx,{1,1},{},Status::unavailable};
    CHECK(adapter.accept(unavailable)==Status::unavailable && adapter.render(ctx->endpoint,0,target)==Status::unavailable);
    CHECK(surface.projected_history_frames()==0 && image.value_count()==0);
}
void vector_coordinate_axes() {
    Storage storage;
    NumericContract contract;contract.dtype=DType::Float32;contract.semantic=Semantic::Numeric;
    contract.channel_order=channels;contract.quantity=Quantity::Amplitude;contract.unit=NumericUnit::RawAmplitude;
    contract.shape={Rank::Vector,{Axis::Bin,Axis::None},{ExtentBounds{3,3},ExtentBounds{}}};
    contract.coordinates[0]={CoordinateMode::Regular,NumericUnit::Hertz,{50,1},{10,1}};
    presentation::NumericViewAdapter adapter({4,{3,3,128,{true,3}},{true,128}});
    FreqChartView frequency;LineChartView line;
    const auto render_index=[&](const auto& ctx,AbstractChartView* chart){
        render_fixture::Target target{chart,nullptr,nullptr,true};target.axes={};return adapter.render(ctx->endpoint,0,target);
    };
    auto deliver=[&](NumericContract c,int node,std::span<const std::byte> coordinate_bytes={}) {
        auto ctx=context(c,node);CHECK(adapter.bind(ctx->endpoint)==Status::ok);
        const std::array<float,3> values{.03f,.2f,.1f};
        const std::array<PlaneLayout,1> planes{{{0,{4,4}}}};
        NumericView numeric{&c,std::as_bytes(std::span(values)),planes,{3,1},{},{{coordinate_bytes,{}}}};
        FrameStamp stamp;stamp.sequence=1;
        pipeline::DeliveredNumericFrame event;
        CHECK(pipeline::retain_numeric_delivery(ctx,{1,1},{stamp,numeric},Status::ok,limits,storage.p,storage.m,event)==Status::ok);
        CHECK(adapter.accept(event)==Status::ok);
        CHECK(render_index(ctx,&frequency)==Status::ok);
        CHECK(render_index(ctx,&line)==Status::ok);
        render_fixture::Target fixture{&frequency,nullptr,nullptr,true};
        auto explicitTarget=static_cast<presentation::NumericRenderTarget>(fixture);
        explicitTarget.frequency_grid={true,10,1,"Frequency bin (X)","Frequency, Hz (X)"};
        CHECK(adapter.render(ctx->endpoint,0,explicitTarget)==Status::ok);
        CHECK(frequency.x_title()=="Frequency, Hz (X)" && frequency.projected_x_coordinate(0)==10 && frequency.projected_x_coordinate(2)==30);
        explicitTarget.frequency_grid.step=20;
        CHECK(adapter.render(ctx->endpoint,0,explicitTarget)==Status::ok && frequency.projected_x_coordinate(0)==20 && frequency.projected_x_coordinate(2)==60);
        explicitTarget.frequency_grid.step=0;
        CHECK(adapter.render(ctx->endpoint,0,explicitTarget)==Status::ok && frequency.projected_x_coordinate(0)==0 && frequency.projected_x_coordinate(2)==2 && frequency.x_title()=="Frequency bin (X)");
        CHECK(render_index(ctx,&frequency)==Status::ok);
        if(c.coordinates[0].unit==NumericUnit::Hertz){
            render_fixture::Target unbound{&line,nullptr,nullptr,true};unbound.axes={};
            CHECK(adapter.render(ctx->endpoint,0,unbound)==Status::ok);
            CHECK(line.x_title()=="Value index (X)");
            CHECK(line.projected_x_coordinate(0)==0 && line.projected_x_coordinate(2)==2);
            // Binding changes are presentation-only: the same owning frame is
            // immediately reusable, with no data or coordinate mutation.
            CHECK(render_index(ctx,&line)==Status::ok);
        }
        return ctx;
    };

    const auto regularContext=deliver(contract,10);
    for(AbstractChartView* chart:{static_cast<AbstractChartView*>(&frequency),static_cast<AbstractChartView*>(&line)}) {
        CHECK(chart->x_range().first==0 && chart->x_range().second==2);
        CHECK(chart->x_title()=="Value index (X)");
        CHECK(chart->projected_x_coordinate(0)==0 && chart->projected_x_coordinate(2)==2);
    }
    render_fixture::Target coordinatesTarget{&frequency,nullptr,nullptr,true};
    coordinatesTarget.plot_presentation.vector_x_title="Coordinate (X)";
    CHECK(adapter.render(regularContext->endpoint,0,coordinatesTarget)==Status::ok);
    CHECK(frequency.x_title()=="Coordinate (X)" && frequency.projected_x_coordinate(0)==50 && frequency.projected_x_coordinate(2)==70);
    contract.coordinates[0]={CoordinateMode::Regular,NumericUnit::Hertz,{701,10},{1,20}};
    deliver(contract,13);
    CHECK(frequency.x_range().first==0 && frequency.x_range().second==2);
    CHECK(frequency.projected_x_coordinate(1)==1);
    std::array<std::byte,24> coordinates{};
    const std::array<double,3> hz{70,95,180};
    for(size_t i=0;i<hz.size();++i) {
        const auto bits=std::bit_cast<std::uint64_t>(hz[i]);
        for(unsigned byte=0;byte<8;++byte)coordinates[i*8+byte]=std::byte((bits>>(byte*8))&255);
    }
    contract.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Hertz};
    const auto explicitContext=deliver(contract,11,coordinates);
    CHECK(frequency.x_range().first==0 && frequency.x_range().second==2);
    CHECK(line.projected_x_coordinate(1)==1 && frequency.projected_x_coordinate(1)==1);
    CHECK(adapter.render(explicitContext->endpoint,0,coordinatesTarget)==Status::ok);
    CHECK(frequency.projected_x_coordinate(0)==70 && frequency.projected_x_coordinate(1)==95 && frequency.projected_x_coordinate(2)==180);
    CHECK(render_index(explicitContext,&frequency)==Status::ok);
    frequency.resize(400,240);line.resize(400,240);frequency.show();line.show();
    const auto hasCentre=[](AbstractChartView& view) {
        const auto points=view.projected_points();
        return std::any_of(points.begin(),points.end(),[](auto point){
            return point.x()==1 && std::abs(point.y()-.2)<1e-6;
        });
    };
    QElapsedTimer timer;timer.start();
    while(timer.elapsed()<1500 && (!hasCentre(frequency) || !hasCentre(line))) {
        QCoreApplication::processEvents();QThread::msleep(2);
    }
    CHECK(hasCentre(frequency) && hasCentre(line));
    frequency.hide();line.hide();

    contract.semantic=Semantic::MelEnergy;contract.shape.axes[0]=Axis::Band;contract.coordinates[0]={};
    deliver(contract,12);
    CHECK(frequency.x_title()=="Value index (X)");
    CHECK(line.x_title()=="Value index (X)");
    CHECK(frequency.x_range().first==0 && frequency.x_range().second==2);
    CHECK(line.projected_x_coordinate(2)==2);
    // The axis is structural; descriptive tags cannot relabel identical geometry.
    contract.semantic=Semantic::Numeric;deliver(contract,12);
    CHECK(frequency.x_title()=="Value index (X)" && line.x_title()=="Value index (X)");
}
void fft_product_and_phase_axes() {
    using namespace domain;using namespace dataflow_runtime;
    constexpr FrameLimits frameLimits{1,32768,262144,16384,256,32768};
    const ProcessingAdmissionLimits admission{32,64,16,16,256,65536,frameLimits};
    NumericRuntimeProfile profile{admission,{{4194304,4194304,4194304,131072},0,16777216},{32768,1,262144,262144,262144},{true,true,true}};profile.music_enabled=true;
    auto own=[&](const NumericContract& c){OwnedProcessingContract out;CHECK(own_processing_contract(c,frameLimits,65536,out)==Status::ok);return out;};
    FreqChartView magnitude,phase;
    presentation::NumericViewAdapter adapter({4,{4,1024,32768,{true,4096}},{true,1024}});
    // Same endpoints and actual renderer instances survive a new Fs/N contract.
    for(const auto [size,rate]:{std::pair{512u,48000u},std::pair{1024u,32000u}}) {
        NumericContract input;input.channel_order=channels;input.dtype=DType::Float32;input.representation.alignment=alignof(float);
        input.shape={Rank::Vector,{Axis::Sample,Axis::None},{ExtentBounds{size,size},ExtentBounds{}}};input.clock={"axis-fixture",{1,rate},rate};
        ConfiguredProcessingNode source;source.id=1;source.kind="NetworkInput";source.outputs={{0,PortDirection::Output,own(input)}};
        ConfiguredProcessingNode fft;fft.id=3;fft.kind="RealFourier";fft.parameters=TransformProcessingParameters{{{"fft_size",NumericLiteral{std::uint64_t(size)}}}};fft.inputs={{0,PortDirection::Input,own(input)}};
        std::array<OwnedProcessingContract,4> coefficients;CHECK(fourier_output_contracts(fft,input,coefficients)==Status::ok);
        for(PortId id=0;id<4;++id)fft.outputs.push_back({id,PortDirection::Output,coefficients[id]});
        ConfiguredProcessingNode polar;polar.id=4;polar.kind="AmplitudePhase";polar.parameters=TransformProcessingParameters{{{"fft_size",NumericLiteral{std::uint64_t(size)}},{"sample_rate",NumericLiteral{std::uint64_t(rate)}}}};
        polar.inputs={{0,PortDirection::Input,coefficients[0]},{1,PortDirection::Input,coefficients[1]}};
        std::array<OwnedProcessingContract,2> projected;CHECK(amplitude_phase_output_contracts(polar,std::get<NumericContract>(coefficients[0].view()),projected)==Status::ok);
        polar.outputs={{0,PortDirection::Output,projected[0]},{1,PortDirection::Output,projected[1]}};
        const auto amplitudeContract=std::get<NumericContract>(projected[0].view()),phaseContract=std::get<NumericContract>(projected[1].view());
        CHECK(phaseContract.coordinates==amplitudeContract.coordinates && phaseContract.unit==NumericUnit::Radians);
        auto gains=amplitudeContract;gains.quantity=Quantity::Unspecified;gains.unit=NumericUnit::Values;gains.coordinates={};gains.clock={};
        ConfiguredProcessingNode gainSource;gainSource.id=2;gainSource.kind="NetworkInput";gainSource.outputs={{0,PortDirection::Output,own(gains)}};
        ConfiguredProcessingNode product;product.id=5;product.kind="ArrayMultiply";product.parameters=TransformProcessingParameters{};
        product.inputs={{0,PortDirection::Input,projected[0]},{1,PortDirection::Input,own(gains)}};product.outputs={{0,PortDirection::Output,projected[0]}};
        NumericProcessingGraph graph;graph.graph_id="fft-axis-pipeline";graph.nodes={source,gainSource,fft,polar,product};
        graph.edges={{1,3,0,0},{3,4,0,0},{3,4,1,1},{4,5,0,0},{2,5,0,1}};
        std::shared_ptr<const PreparedGraph> plan;std::unique_ptr<RuntimeContext> state;CHECK(PreparedGraph::prepare_numeric(graph,profile,plan,state)==Status::ok);if(!plan)return;
        struct Observation {
            const FrameLimits& limits;
            struct Arena {
                alignas(64) std::array<std::byte,65536> payload{},metadata{};
                std::array<BufferSlot,8> payloadSlots{},metadataSlots{};
                BufferPool p{payload,payloadSlots,8192},m{metadata,metadataSlots,8192};
            };
            std::shared_ptr<Arena> arena=std::make_shared<Arena>();
            pipeline::DeliveredNumericFrame spectrum,phase;
            static void output(void* self,const NumericPortFrameView& event){
                auto& o=*static_cast<Observation*>(self);const bool isPhase=event.identity.node==4 && event.identity.port==1;
                if(!event.frame.stamp.available || (!isPhase && event.identity.node!=5))return;
                auto context=std::make_shared<pipeline::NumericDeliveryContext>();context->endpoint={"fft-axis-pipeline",event.identity.node,PortDirection::Output,event.identity.port};
                context->retention_owner=o.arena;
                CHECK(own_processing_contract(*std::get<NumericView>(event.frame.payload).contract,o.limits,65536,context->contract)==Status::ok);
                CHECK(pipeline::retain_numeric_delivery(context,{1,1},event.frame,event.status,o.limits,o.arena->p,o.arena->m,isPhase?o.phase:o.spectrum)==Status::ok);
            }
        } observed{frameLimits};
        NumericRuntimeCallbacks callbacks{Observation::output,nullptr,nullptr,&observed};
        std::vector<float> samples(size),weights(size/2,1.f);
        for(std::size_t t=0;t<size;++t)for(unsigned tone=1;tone<=5;++tone)samples[t]+=.05f*std::cos(6.283185307179586*double(tone*7)*double(t)/size);
        const PlaneLayout planeLayout{0,{sizeof(float),0}};FrameStamp stamp;stamp.sequence=size;stamp.producer_epoch=1;stamp.has_sample_interval=true;stamp.sample_count=size;
        NumericView weightView{&gains,std::as_bytes(std::span(weights)),std::span(&planeLayout,1),{size/2,1},{},{}};
        CHECK(plan->process_numeric_ports(2,{stamp,weightView},*state,{},callbacks)==Status::ok);
        NumericView sampleView{&input,std::as_bytes(std::span(samples)),std::span(&planeLayout,1),{size,1},{},{}};
        CHECK(plan->process_numeric_ports(1,{stamp,sampleView},*state,{},callbacks)==Status::ok);
        for(const auto pair:{std::pair{&observed.spectrum,&magnitude},std::pair{&observed.phase,&phase}}){
            CHECK(pair.first->context);if(!pair.first->context)continue;
            const auto endpoint=pair.first->context->endpoint;CHECK(adapter.bind(endpoint)==Status::ok);CHECK(adapter.accept(*pair.first)==Status::ok);
            render_fixture::Target target{pair.second,nullptr,nullptr,true};target.axes={};target.plot_presentation.vector_x_title="Frequency bin (X)";
            CHECK(adapter.render(endpoint,0,target)==Status::ok);
            CHECK(pair.second->x_title()=="Frequency bin (X)");
            CHECK(pair.second->projected_x_coordinate(0)==0);
            CHECK(pair.second->projected_x_coordinate(size/2-1)==double(size/2-1));
            CHECK(pair.second->x_range().first==0 && pair.second->x_range().second==double(size/2-1));
        }
    }
}
void actual_scene_aspect_projection() {
    SpectrogramGraph surface;
    surface.set_history_dimensions(400,50);
    auto options=surface.surface_settings();options["y_range_mode"]="manual";options["y_min"]=0;options["y_max"]=3;
    options["scene_time_aspect"]=3;options["scene_band_aspect"]=.75;
    surface.set_surface_settings(options);
    const auto camera=surface.camera_state();
    surface.update_frame_history({std::vector<float>(50,.5f)});
    for(int crop:{1,2,100}) {
        surface.set_presentation_state({{"ranges",QJsonArray{0,crop,0,100}}});
        const auto scene=surface.projected_scene_geometry();
        CHECK(scene.scaleX==3 && scene.scaleZ==.75f && scene.yMin==0 && scene.yMax==3);
        CHECK(std::abs(scene.xMax-399.f*crop/100.f)<1e-5f);
        CHECK(scene.point(scene.xMax,scene.zMax,3).y()==1);
        CHECK(surface.camera_state()==camera);
    }
    surface.set_history_dimensions(800,2);
    CHECK(surface.projected_scene_geometry().scaleX==3 && surface.projected_scene_geometry().scaleZ==.75f);
    surface.set_history_dimensions(400,50);
    surface.set_presentation_state({{"ranges",QJsonArray{0,1,0,100}}});
    options["scene_aspect_mode"]="sample_counts";surface.set_surface_settings(options);
    const auto oldPolicy=surface.projected_scene_geometry();
    CHECK(std::abs(oldPolicy.scaleX-2.f*3.99f/49.f)<1e-6f && oldPolicy.scaleZ==2);
    float value=0;CHECK(surface.projected_history_sample(0,0,value) && value==.5f);
    CHECK(surface.y_range().first==0 && surface.y_range().second==3 && surface.camera_state()==camera);
}
void shared_matrix_ownership() {
    NumericMatrixSnapshot::Matrix values{{1,2,3},{4,5,6}};
    auto snapshot=NumericMatrixSnapshot::take(std::move(values));
    CHECK(snapshot.size()==2 && snapshot.front().size()==3 && snapshot.sample_count()==6);
    auto transposed=snapshot.transposed();
    CHECK(transposed.storage_identity()==snapshot.storage_identity());
    CHECK(transposed.size()==3 && transposed.front().size()==2 && transposed[2][1]==6);
    CHECK(spectrogramHistorySample(transposed,1,0,2)==5);
    CHECK(spectrogramPeakSample(transposed,0,3,0,2,2).value==6);
    NumericImageView image;SpectrogramGraph surface;
    image.set_snapshot(snapshot);surface.update_snapshot(transposed);
    CHECK(image.projection_storage_identity()==surface.projection_storage_identity());
    auto equal=NumericMatrixSnapshot::take({{1,2,3},{4,5,6}});
    surface.update_snapshot(equal.transposed());image.set_snapshot(equal);
    CHECK(surface.projection_storage_identity()==equal.storage_identity());
    CHECK(surface.projection_storage_identity()==image.projection_storage_identity());
    surface.set_paused(true);
    auto equal_pending=NumericMatrixSnapshot::take({{1,2,3},{4,5,6}});
    surface.update_snapshot(equal_pending.transposed());surface.set_paused(false);
    CHECK(surface.projection_storage_identity()==equal_pending.storage_identity());
    surface.set_paused(true);
    const auto* frozen=surface.projection_storage_identity();
    for(int i=1;i<=50;++i)surface.update_snapshot(NumericMatrixSnapshot::take({{float(i)}}).transposed());
    CHECK(surface.projection_storage_identity()==frozen);
    snapshot.clear();transposed.clear(); // Widgets must own their snapshot, not borrow these locals.
    surface.set_paused(false);
    float value=0;CHECK(surface.projected_history_sample(0,0,value) && value==50);
    CHECK(image.matrix_sample(1,2,value) && value==6);
    surface.update_snapshot(NumericMatrixSnapshot::take({{1,2},{3}}));
    CHECK(surface.projected_history_frames()==0);
    image.set_snapshot(NumericMatrixSnapshot::take({{std::numeric_limits<float>::infinity()}}));
    CHECK(image.value_count()==0);
}
void atomic_pause_snapshot() {
    SpectrogramGraph surface;
    SpectrogramAxisMetadata original;original.time_coordinates=original.column_coordinates=true;
    original.ages={0,.02};original.columns={100,200};
    surface.set_history_dimensions(400,2);CHECK(surface.set_axis_metadata(original));
    surface.set_projection_status("old status");surface.update_frame_history({{.03f,.04f},{.05f,.06f}});
    const auto revision=surface.history_projection_revision();
    surface.set_paused(true);
    SpectrogramAxisMetadata changed=original;changed.ages={0,.1,.3};changed.columns={500};
    surface.set_history_dimensions(800,1);CHECK(surface.set_axis_metadata(changed));
    surface.set_projection_status("new status");surface.update_frame_history({{1.f},{2.f},{3.f}});
    CHECK(surface.projected_history_capacity()==400 && surface.projected_history_bands()==2);
    CHECK(surface.projected_axis_metadata()==original && surface.projected_status()=="old status");
    CHECK(surface.history_projection_revision()==revision && surface.projected_history_frames()==2);
    float value=0;CHECK(surface.projected_history_sample(0,0,value) && value==.03f);
    surface.set_paused(false);
    CHECK(surface.projected_history_capacity()==800 && surface.projected_history_bands()==1);
    CHECK(surface.projected_axis_metadata()==changed && surface.projected_status()=="new status");
    CHECK(surface.projected_history_frames()==3 && surface.projected_history_sample(2,0,value) && value==3.f);
    surface.set_paused(true);surface.update_frame_history({});
    CHECK(surface.projected_history_frames()==3);
    surface.set_paused(false);CHECK(surface.projected_history_frames()==0);
    // A reset/contract transition received during pause must not clear only the
    // values while leaving frozen axes from another projection revision.
    surface.set_presentation_state({{"pause",true}});
    CHECK(surface.paused());surface.set_presentation_state({});CHECK(!surface.paused());
}
void fixed_capacity_history_and_resize() {
    Storage storage;NumericContract history;history.semantic=Semantic::History;history.history_element=Semantic::Numeric;
    history.channel_order=channels;history.shape={Rank::Matrix,{Axis::Time,Axis::Declared},{ExtentBounds{0,4},ExtentBounds{2,2}}};
    history.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds};
    auto ctx=context(history,25);
    presentation::NumericViewAdapter adapter({1,{8,2,136,{true,2}},{true,128}});adapter.bind(ctx->endpoint);
    SpectrogramGraph surface;QLabel label;render_fixture::Target target{nullptr,&surface,&label,true};
    const std::array<PlaneLayout,1> full_planes{{{0,{8,4}}}};
    const std::array<float,8> zero{};const std::array<std::byte,32> zero_times{};
    NumericView full{&history,std::as_bytes(std::span(zero)),full_planes,{4,2},{4,0},{{zero_times,{}}}};
    FrameStamp stamp;stamp.producer_epoch=7;
    pipeline::DeliveredNumericFrame initial{ctx,{1,1},{},Status::unavailable};
    CHECK(adapter.accept_history_snapshot(initial,0,0)==Status::unavailable);
    CHECK(adapter.render(ctx->endpoint,0,target)==Status::unavailable);
    CHECK(surface.projected_history_frames()==0); // No GUI-generated matrix without core payload.
    CHECK(retain_frame({stamp,full},limits,storage.p,storage.m,initial.frame)==Status::ok);
    CHECK(adapter.accept_history_snapshot(initial,0,0)==Status::unavailable);
    CHECK(adapter.render(ctx->endpoint,0,target)==Status::unavailable);
    CHECK(surface.projected_history_frames()==4 && surface.projected_history_capacity()==4);
    CHECK(surface.projected_status().contains("unavailable"));
    initial.status=Status::ok;
    CHECK(adapter.accept_history_snapshot(initial,0,0)==Status::ok);
    CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok);
    CHECK(surface.projected_history_frames()==4 && surface.projected_history_capacity()==4 && label.text().contains("0"));
    CHECK(!surface.projected_axis_metadata().time_coordinates && surface.projected_axis_metadata().ages==std::vector<double>({0,1,2,3}));
    const auto camera=surface.camera_state();const auto footprint=surface.projected_scene_geometry();
    auto sample=[&](std::size_t frame){float value=-1;CHECK(surface.projected_history_sample(frame,0,value));return value;};
    for(std::size_t i=0;i<4;++i)CHECK(sample(i)==0);
    std::vector<std::pair<std::uint64_t,double>> upstream;
    const auto send=[&](std::uint64_t ordinal,std::uint64_t filled,double seconds) {
        upstream.emplace_back(ordinal,seconds);
        if(upstream.size()>filled)upstream.erase(upstream.begin(),upstream.end()-filled);
        const auto capacity=history.shape.extents[0].maximum;
        std::vector<float> values(capacity*2);std::vector<std::byte> times(capacity*8);
        for(std::size_t i=0;i<upstream.size();++i) {
            const auto row=capacity-upstream.size()+i;
            values[row*2]=float(upstream[i].first);values[row*2+1]=float(upstream[i].first)+.5f;
            const auto bits=std::bit_cast<std::uint64_t>(upstream[i].second);
            for(unsigned b=0;b<8;++b)times[row*8+b]=std::byte((bits>>(8*b))&255);
        }
        // A hot notice alone never appends a graph-owned row.
        pipeline::DeliveredNumericHistoryRow notice;notice.compact.context=ctx;notice.compact.delivery={ordinal,1};
        CHECK(adapter.accept(notice)==Status::would_block);
        pipeline::DeliveredNumericFrame snapshot{ctx,{ordinal,1},{},Status::ok};stamp.sequence=ordinal;
        NumericView view{&history,std::as_bytes(std::span(values)),full_planes,{capacity,2},{capacity,0},{{times,{}}}};
        CHECK(retain_frame({stamp,view},limits,storage.p,storage.m,snapshot.frame)==Status::ok);
        CHECK(adapter.accept_history_snapshot(snapshot,ordinal,filled)==Status::ok);
        CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok);
    };
    send(1,1,1);CHECK(surface.projected_history_frames()==4 && sample(0)==0 && sample(3)==1);
    CHECK(!surface.projected_axis_metadata().time_coordinates && surface.projected_axis_metadata().ages==std::vector<double>({0,1,2,3}));
    CHECK(surface.projected_axis_metadata().age(3)==3);
    CHECK(surface.projected_scene_geometry().scaleX==footprint.scaleX && surface.projected_scene_geometry().scaleZ==footprint.scaleZ);
    send(2,2,2);CHECK(!surface.projected_axis_metadata().time_coordinates && surface.projected_axis_metadata().ages==std::vector<double>({0,1,2,3}));
    send(3,3,4);CHECK(!surface.projected_axis_metadata().time_coordinates && surface.projected_axis_metadata().ages==std::vector<double>({0,1,2,3}));
    history.shape.extents[0].maximum=8;ctx=context(history,25);send(4,4,7);
    CHECK(surface.projected_history_frames()==8 && surface.projected_history_capacity()==8);
    for(std::size_t i=0;i<4;++i)CHECK(sample(i)==0);
    CHECK(sample(4)==1 && sample(5)==2 && sample(6)==3 && sample(7)==4);
    CHECK(!surface.projected_axis_metadata().time_coordinates && surface.projected_axis_metadata().ages==std::vector<double>({0,1,2,3,4,5,6,7}));
    CHECK(surface.projected_axis_metadata().age(7)==7);
    history.shape.extents[0].maximum=2;ctx=context(history,25);send(5,2,11);
    CHECK(surface.projected_history_frames()==2 && sample(0)==4 && sample(1)==5);
    CHECK(surface.projected_axis_metadata().time_coordinates && surface.projected_axis_metadata().ages==std::vector<double>({0,4}));
    CHECK(surface.camera_state()==camera && surface.projected_scene_geometry().scaleX==footprint.scaleX);
    NumericView unavailable{&history};stamp.available=false;
    pipeline::DeliveredNumericFrame marker{ctx,{6,1},{},Status::unavailable};
    CHECK(retain_frame({stamp,unavailable},limits,storage.p,storage.m,marker.frame)==Status::ok);
    CHECK(adapter.accept_history_snapshot(marker,4,2)==Status::would_block);
    CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok);
    CHECK(surface.projected_history_frames()==2 && sample(0)==4 && sample(1)==5);
    CHECK(adapter.accept_history_snapshot(marker,5,2)==Status::unavailable);
    CHECK(adapter.render(ctx->endpoint,0,target)==Status::unavailable);
    CHECK(surface.projected_history_frames()==0 && surface.projected_status().contains("unavailable"));

}
void cold_history_resynchronization() {
    Storage storage;NumericContract history;history.dtype=DType::UInt64;history.semantic=Semantic::History;
    history.history_element=Semantic::Numeric;history.channel_order=channels;
    history.shape={Rank::Matrix,{Axis::Time,Axis::Bin},{ExtentBounds{0,3},ExtentBounds{2,2}}};
    history.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds};
    history.coordinates[1]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Hertz};
    auto ctx=context(history,4);
    std::array<std::uint64_t,6> values{3,4,1,2,2,3}; // physical wrap: oldest is row one
    const std::array<PlaneLayout,1> planes{{{0,{16,8}}}};
    const auto coordinates=[](auto values) {
        std::array<std::byte,std::tuple_size_v<decltype(values)>*8> bytes{};
        for(std::size_t i=0;i<values.size();++i) {
            const auto bits=std::bit_cast<std::uint64_t>(values[i]);
            for(unsigned byte=0;byte<8;++byte)bytes[i*8+byte]=std::byte((bits>>(byte*8))&255);
        }
        return bytes;
    };
    const auto times=coordinates(std::array<double,3>{.5,.75,1.});
    const auto columns=coordinates(std::array<double,2>{70.,95.});
    NumericView full{&history,std::as_bytes(std::span(values)),planes,{3,2},{3,1},{{times,columns}}};
    FrameStamp stamp;stamp.sequence=500;stamp.producer_epoch=8;stamp.generation=17;
    pipeline::DeliveredNumericFrame snapshot{ctx,{3,5},{},Status::ok};
    CHECK(retain_frame({stamp,full},limits,storage.p,storage.m,snapshot.frame)==Status::ok);
    // Hot callbacks still refuse the full matrix.
    pipeline::DeliveredNumericFrame forbidden;
    CHECK(pipeline::retain_numeric_delivery(ctx,{3,5},{stamp,full},Status::ok,limits,storage.p,storage.m,forbidden)==Status::incompatible_contract);
    pipeline::HistoryPresentation trail({3,2,56,{true,2}});
    CHECK(trail.accept_snapshot(snapshot,80,3)==Status::ok);
    const auto rows=trail.render_snapshot();CHECK(rows.size()==3 && rows[0][0]==1 && rows[2][1]==4);
    NumericMatrixSnapshot::Matrix reusable(8,std::vector<float>(9,99));
    trail.render_snapshot_into(reusable);CHECK(reusable==rows);
    const auto* reused_storage=reusable[0].data();trail.render_snapshot_into(reusable);
    CHECK(reusable==rows && reusable[0].data()==reused_storage);
    const auto axes=trail.render_axes();CHECK(axes.seconds && axes.ages==std::vector<double>({0,.25,.5}));
    CHECK(axes.column_coordinates && axes.column_unit==NumericUnit::Hertz && axes.columns==std::vector<double>({70,95}));
    presentation::NumericViewAdapter adapter({1,{3,2,56,{true,2}},{true,128}});adapter.bind(ctx->endpoint);
    SpectrogramGraph surface;QLabel label;render_fixture::Target target{nullptr,&surface,&label,false};
    target.plot_presentation.column_title="Frequency, Hz (Z)";target.plot_presentation.column_index_title="Frequency bin (Z)";
    target.plot_presentation.row_title="Time delay, seconds (X)";target.plot_presentation.row_index_title="Temporal frame (X)";
    CHECK(adapter.accept_history_snapshot(snapshot,80,3)==Status::ok);CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok);
    CHECK(surface.projected_history_frames()==0);target.visible=true;
    CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok);float sample=0;
    CHECK(surface.projected_history_frames()==3 && surface.projected_history_sample(0,0,sample) && sample==1);
    CHECK(surface.projected_axis_metadata().time_title_key=="Time delay, seconds (X)");
    CHECK(surface.projected_axis_metadata().column_title_key=="Frequency, Hz (Z)");
    CHECK(surface.projected_axis_metadata().ages==std::vector<double>({0,.25,.5}));
    CHECK(surface.projected_axis_metadata().columns==std::vector<double>({70,95}));
    target.axes={};CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok);
    CHECK(!surface.projected_axis_metadata().time_coordinates && !surface.projected_axis_metadata().column_coordinates);
    CHECK(surface.projected_axis_metadata().time_title_key=="Temporal frame (X)");
    CHECK(surface.projected_axis_metadata().column_title_key=="Frequency bin (Z)");
    CHECK(surface.projected_axis_metadata().ages==std::vector<double>({0,1,2}));
    CHECK(surface.projected_axis_metadata().columns==std::vector<double>({0,1}));
    CHECK(surface.projected_history_sample(0,0,sample) && sample==1);
    target.axes={true,true};CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok);
    CHECK(surface.projected_axis_metadata().columns==std::vector<double>({70,95}));
    auto row=history;row.semantic=Semantic::Numeric;row.history_element=Semantic::Numeric;
    row.shape={Rank::Vector,{Axis::Bin,Axis::None},{ExtentBounds{2,2},ExtentBounds{}}};row.coordinates={history.coordinates[1],AxisCoordinates{}};
    const std::array<PlaneLayout,1> row_planes{{{0,{8,8}}}};const std::array<std::uint64_t,2> next_values{4,5};
    NumericView compact{&row,std::as_bytes(std::span(next_values)),row_planes,{2,1},{},{{columns,{}}}};
    pipeline::DeliveredNumericHistoryRow next;next.event=pipeline::NumericHistoryEvent::Row;next.capacity=next.filled_count=3;
    next.accepted_row_ordinal=81;next.has_time_coordinate=true;next.time_coordinate=coordinates(std::array<double,1>{1.25});
    stamp.sequence=900;
    CHECK(pipeline::retain_numeric_delivery(ctx,{3,5},{stamp,compact},Status::ok,limits,storage.p,storage.m,next.compact,true)==Status::ok);
    const std::array<std::uint64_t,6> fresh_values{2,3,3,4,4,5};
    const auto fresh_times=coordinates(std::array<double,3>{.75,1.,1.25});
    NumericView fresh{&history,std::as_bytes(std::span(fresh_values)),planes,{3,2},{3,0},{{fresh_times,columns}}};
    pipeline::DeliveredNumericFrame fresh_snapshot{ctx,{3,5},{},Status::ok};
    CHECK(retain_frame({stamp,fresh},limits,storage.p,storage.m,fresh_snapshot.frame)==Status::ok);
    CHECK(trail.accept_snapshot(fresh_snapshot,81,3)==Status::ok);
    CHECK(trail.render_snapshot()[0][0]==2 && trail.render_snapshot()[2][1]==5);
    CHECK(trail.render_axes().ages==std::vector<double>({0,.25,.5}));
    surface.set_paused(true);label.setText("frozen");
    CHECK(adapter.accept(next)==Status::would_block);
    CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok);
    CHECK(adapter.accept_history_snapshot(fresh_snapshot,81,3)==Status::ok);
    CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok);
    CHECK(surface.projected_history_sample(0,0,sample) && sample==1);
    CHECK(label.text()=="frozen");
    CHECK(surface.projected_axis_metadata().ages==std::vector<double>({0,.25,.5}));
    surface.set_paused(false);CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok);
    CHECK(label.text()!="frozen");
    CHECK(surface.projected_history_sample(0,0,sample) && sample==2);
    CHECK(surface.projected_history_sample(2,1,sample) && sample==5);
    CHECK(trail.accept_snapshot(snapshot,80,3)==Status::would_block);
    CHECK(adapter.accept_history_snapshot(snapshot,80,3)==Status::would_block);
    CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok);
    CHECK(surface.projected_history_sample(2,1,sample) && sample==5);
    auto stale=snapshot;stale.delivery={2,5};CHECK(trail.accept_snapshot(stale,100,3)==Status::would_block);
    CHECK(trail.accept_snapshot(snapshot,2,3)==Status::malformed);CHECK(trail.render_snapshot()[2][1]==5);
    pipeline::HistoryPresentation refused({3,2,55,{true,2}});
    CHECK(refused.accept_snapshot(snapshot,80,3)==Status::buffer_too_small && refused.size()==0);
}
}
void recycled_snapshot_leases() {
    NumericMatrixSnapshotPool pool;
    const auto make=[&](float value){return pool.fill([&](auto& rows){rows.resize(2);for(auto& row:rows)row.assign(3,value);});};
    auto first=make(1),second=make(2),third=make(3);
    const auto identity=first.storage_identity();auto transposed=first.transposed();
    const auto fallbacks=NumericMatrixSnapshotPool::fallbacks;
    auto fourth=make(4);
    CHECK(NumericMatrixSnapshotPool::fallbacks==fallbacks+1);
    CHECK(first[0][0]==1 && transposed[2][1]==1 && second[1][2]==2 && third[0][1]==3);
    first.clear();transposed.clear();auto reused=make(5);
    CHECK(reused.storage_identity()==identity && reused[1][2]==5 && fourth[0][0]==4);
    pool.clear();CHECK(reused[1][2]==5 && second[0][0]==2);
    CHECK(pool.fill([](auto& rows){rows={{1,2},{3}};}).empty());
    CHECK(pool.fill([](auto& rows){rows={{std::numeric_limits<float>::infinity()}};}).empty());
    CHECK(pool.fill([](auto& rows){rows.clear();}).empty());
}
void piano_projection(){
    Storage storage;NumericContract contract;contract.dtype=DType::Float32;contract.channel_order=channels;contract.unit=NumericUnit::Values;
    contract.shape={Rank::Vector,{Axis::Band,Axis::None},{ExtentBounds{13,13},ExtentBounds{}}};contract.representation.alignment=alignof(float);contract.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Hertz};
    auto ctx=context(contract,25);std::array<float,13> values{};values[9]=1;values[1]=.5f;std::array<double,13> hz{};for(unsigned i=0;i<13;++i)hz[i]=442.*std::pow(2.,(double(i)+60-69)/12);
    const PlaneLayout plane{0,{4,0}};NumericView view{&contract,std::as_bytes(std::span(values)),{&plane,1},{13,1},{},{}};view.coordinates.axes[0]=std::as_bytes(std::span(hz));
    FrameStamp stamp;stamp.sequence=1;stamp.producer_epoch=9;pipeline::DeliveredNumericFrame frame;
    CHECK(pipeline::retain_numeric_delivery(ctx,{1,1},{stamp,view},Status::ok,limits,storage.p,storage.m,frame)==Status::ok);
    presentation::NumericViewAdapter adapter({4,{3,2,56,{true,2}},{true,128}});CHECK(adapter.bind(ctx->endpoint)==Status::ok);
    NumericPianoView piano;piano.resize(900,300);piano.set_first_midi(60);
    presentation::NumericPianoPort port;port.set_values=[&](const auto& v){return piano.set_values(v);};port.set_frequencies=[&](const auto& v){return piano.set_frequencies(v);};port.caption=[&](const char* key,QStringList args,bool,bool){auto text=*key?ui::text(key):QString{};for(const auto& a:args)text=text.arg(a);piano.set_projection_status(text);};
    presentation::NumericRenderTarget target;target.visible=true;target.axes={true,true};target.piano=&port;target.presentation.piano_frequency_coordinates=true;
    CHECK(adapter.accept(frame)==Status::ok && adapter.render(ctx->endpoint,0,target)==Status::ok);
    CHECK(piano.value_count()==13 && piano.note_value(69)==1 && piano.note_value(61)==.5f && piano.note_name(69)=="A4");
    CHECK(piano.key_rect(61).height()<piano.key_rect(60).height() && piano.key_rect(61).width()<piano.key_rect(60).width());
    piano.show();
    const auto snapshot=[&]{
        QElapsedTimer deadline;deadline.start();
        while(piano.presentation_model()->state().value("pianoRevision").toULongLong()!=piano.presentation_revision() && deadline.elapsed()<1000){QCoreApplication::processEvents();QThread::msleep(1);}
        CHECK(piano.presentation_model()->state().value("pianoRevision").toULongLong()==piano.presentation_revision());
        const auto image=piano.grabFramebuffer();QCoreApplication::processEvents();return image;
    };
    CHECK(!snapshot().isNull());
    const auto unchangedRevision=piano.presentation_revision();unsigned duplicatePublications=0;
    const auto duplicateConnection=QObject::connect(piano.presentation_model(),&QuickPlotModel::changed,&piano,[&]{++duplicatePublications;});
    for(unsigned repeat=0;repeat<200;++repeat){CHECK(piano.set_values(std::vector<float>(values.begin(),values.end())));CHECK(piano.set_frequencies(std::vector<double>(hz.begin(),hz.end())));}
    QCoreApplication::processEvents();
    CHECK(piano.presentation_revision()==unchangedRevision && duplicatePublications==0);
    QObject::disconnect(duplicateConnection);
    const auto oldTheme=ui::Theme::instance().code(),oldLanguage=ui::Language::instance().code();
    for(const auto* language:{"en","ru"})for(const auto* theme:{"light","dark"}){
        ui::Language::instance().set(language);ui::Theme::instance().set(theme);QCoreApplication::processEvents();
        const auto image=snapshot();const auto path=QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QString("../../.agent-workspace/blocks17-piano-%1-%2.png").arg(language,theme));CHECK(!image.isNull() && image.save(path));
        const auto lit=piano.key_rect(69).center().toPoint(),unlit=piano.key_rect(67).center().toPoint();CHECK(image.pixelColor(lit)!=image.pixelColor(unlit));
        const QRect activityArea(12,40,piano.width()-24,piano.height()-80);
        CHECK(piano.set_values(std::vector<float>(13,0)));
        const auto silence=snapshot();
        CHECK(piano.set_values(std::vector<float>(13,1e-12f)));
        const auto floor=snapshot();
        CHECK(piano.peak_value()==1e-12f && piano.note_value(69)==1e-12f);
        CHECK(floor.copy(activityArea)==silence.copy(activityArea));
        std::vector<float> quiet(13,1e-12f);quiet[9]=1e-10f;
        CHECK(piano.set_values(quiet));CHECK(snapshot().copy(activityArea)==silence.copy(activityArea));
        quiet[9]=1e-4f;CHECK(piano.set_values(quiet));
        CHECK(snapshot().pixelColor(lit)!=silence.pixelColor(lit));
        piano.set_activity_floor(1e-3);
        CHECK(snapshot().copy(activityArea)==silence.copy(activityArea));
        CHECK(piano.note_value(69)==1e-4f);piano.set_activity_floor(1e-9);
        // A normal signal still lights its actual key; silence clears it again.
        CHECK(piano.set_values(std::vector<float>(values.begin(),values.end())));
        CHECK(snapshot().pixelColor(lit)!=silence.pixelColor(lit));
        CHECK(piano.set_values(std::vector<float>(13,0)));
        CHECK(snapshot().copy(activityArea)==silence.copy(activityArea));
        CHECK(piano.set_values(std::vector<float>(values.begin(),values.end())));

    }
    snapshot();piano.set_render_visible(false);
    const auto hiddenPublished=piano.presentation_model()->state().value("pianoRevision");
    for(unsigned update=1;update<=100;++update){std::vector<float> latest(13,0);latest[9]=float(update)/100;CHECK(piano.set_values(latest));}
    QCoreApplication::processEvents();
    CHECK(piano.presentation_model()->state().value("pianoRevision")==hiddenPublished && piano.note_value(69)==1);
    piano.set_render_visible(true);CHECK(!snapshot().isNull());
    CHECK(piano.presentation_model()->state().value("levels").toList().value(9).toDouble()==1);
    ui::Theme::instance().set(oldTheme);ui::Language::instance().set(oldLanguage);
    piano.set_first_midi(70);CHECK(piano.value_count()==0);piano.set_first_midi(60);CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok);
    auto unavailable=frame;unavailable.status=Status::unavailable;CHECK(adapter.accept(unavailable)==Status::unavailable);CHECK(adapter.render(ctx->endpoint,0,target)==Status::unavailable && piano.value_count()==0);
    CHECK(!piano.set_values({-1}) && piano.value_count()==0);
}
void value_scale_metadata() {
    for(const auto quantity:{Quantity::SpectrumMagnitude,Quantity::SpectrumPeakAmplitude,Quantity::SpectrumPower,Quantity::SpectrumPsd,Quantity::SpectrumAmplitudeDb,Quantity::SpectrumPowerDb,Quantity::SpectrumPsdDb}){
        Storage storage;NumericContract contract;contract.dtype=DType::Float64;contract.channel_order=channels;contract.quantity=quantity;
        const bool db=quantity==Quantity::SpectrumAmplitudeDb || quantity==Quantity::SpectrumPowerDb || quantity==Quantity::SpectrumPsdDb;
        const bool power=quantity==Quantity::SpectrumPower || quantity==Quantity::SpectrumPsd || quantity==Quantity::SpectrumPowerDb || quantity==Quantity::SpectrumPsdDb;
        contract.unit=db?NumericUnit::Decibels:power?NumericUnit::SquaredAmplitude:NumericUnit::RawAmplitude;
        contract.shape={Rank::Vector,{Axis::Bin,Axis::None},{ExtentBounds{3,3},ExtentBounds{}}};
        auto ctx=context(contract,120);presentation::NumericViewAdapter adapter({1,{3,2,56,{true,2}},{true,128}});CHECK(adapter.bind(ctx->endpoint)==Status::ok);
        std::array<double,3> values{1,.1,.01};const PlaneLayout plane{0,{8,0}};NumericView view{&contract,std::as_bytes(std::span(values)),std::span(&plane,1),{3,1}};
        FrameStamp stamp;stamp.sequence=1;pipeline::DeliveredNumericFrame frame;
        CHECK(pipeline::retain_numeric_delivery(ctx,{1,1},{stamp,view},Status::ok,limits,storage.p,storage.m,frame)==Status::ok);
        LineChartView chart;render_fixture::Target target{&chart,nullptr,nullptr,true};
        CHECK(adapter.accept(frame)==Status::ok);CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok);
        CHECK(!chart.value_scale().source_decibels && chart.value_scale().factor==20);
        target.plot_presentation.value_scale.source_decibels=db;
        target.plot_presentation.value_scale.factor=power?10:20;
        CHECK(adapter.render(ctx->endpoint,0,target)==Status::ok);
        CHECK(chart.value_scale().source_decibels==db && chart.value_scale().factor==(power?10:20));
    }
}
NumericContract generic_negative_history_contract(){
    NumericContract contract;contract.dtype=DType::Float32;contract.channel_order=channels;
    contract.semantic=Semantic::History;contract.history_element=Semantic::Numeric;
    contract.shape={Rank::Matrix,{Axis::Time,Axis::Declared},{ExtentBounds{2,2},ExtentBounds{2,2}}};
    contract.coordinates[1]={CoordinateMode::Regular,NumericUnit::Radians,{-3,1},{2,1}};
    return contract;
}
pipeline::DeliveredNumericFrame generic_negative_history_frame(Storage& storage){
    const auto contract=generic_negative_history_contract();auto ctx=context(contract,87);
    const std::array<float,4> values{1,2,3,4};const std::array<PlaneLayout,1> planes{{{0,{8,4}}}};
    pipeline::DeliveredNumericFrame frame{ctx,{1,1},{},Status::ok};
    CHECK(retain_frame({{},NumericView{&contract,std::as_bytes(std::span(values)),planes,{2,2}}},limits,storage.p,storage.m,frame.frame)==Status::ok);
    return frame;
}
void generic_history_coordinate_mapping(presentation::NumericViewAdapter& adapter,render_fixture::Target& target,const pipeline::EndpointIdentity& endpoint,SpectrogramGraph& surface){
    CHECK(adapter.render(endpoint,0,target)==Status::ok);
    CHECK(surface.projected_axis_metadata().column_title_key=="Column coordinate (Z)" && surface.projected_axis_metadata().columns==std::vector<double>({-3,-1}));
}
void generic_history_index_mapping(presentation::NumericViewAdapter& adapter,render_fixture::Target& target,const pipeline::EndpointIdentity& endpoint,SpectrogramGraph& surface){
    target.axes={};CHECK(adapter.render(endpoint,0,target)==Status::ok);
    CHECK(surface.projected_axis_metadata().column_title_key=="Column index (Z)" && surface.projected_axis_metadata().columns==std::vector<double>({0,1}));
}
void accept_generic_history(presentation::NumericViewAdapter& adapter,const pipeline::DeliveredNumericFrame& frame){
    CHECK(adapter.bind(frame.context->endpoint)==Status::ok);
    CHECK(adapter.accept_history_snapshot(frame,2,2)==Status::ok);
}
void generic_negative_history_coordinates(){
    Storage storage;auto frame=generic_negative_history_frame(storage);
    presentation::NumericViewAdapter adapter({1,{2,2,56,{true,2}},{true,128}});accept_generic_history(adapter,frame);
    SpectrogramGraph surface;NumericImageView image;render_fixture::Target target{nullptr,&surface,nullptr,true,&image};
    generic_history_coordinate_mapping(adapter,target,frame.context->endpoint,surface);
    CHECK(image.source_axis_metadata().column_title_key=="Column coordinate (Z)" && image.source_axis_metadata().columns==std::vector<double>({-3,-1}));
    generic_history_index_mapping(adapter,target,frame.context->endpoint,surface);
}
int main(int argc,char** argv) {
    recycled_snapshot_leases();
    QApplication app(argc,argv);ui::Language::instance().set("en");
#ifdef Q_OS_WIN
    QFontDatabase::addApplicationFont(QDir(qEnvironmentVariable("WINDIR")).filePath("Fonts/segoeui.ttf"));app.setFont(QFont("Segoe UI",9));
#endif
    generic_negative_history_coordinates();
    value_scale_metadata();
    piano_projection();
    for(bool pooled:{false,true}) {
        presentation::RenderPreparationPolicy::snapshot_pool=pooled;
        universal_numeric_ranks();temporal_matrix_axes();hidden_scalar_and_contract_transition();hidden_history_and_markers();cold_history_resynchronization();shared_matrix_ownership();atomic_pause_snapshot();vector_coordinate_axes();fft_product_and_phase_axes();actual_scene_aspect_projection();scalar_timed_readout();fixed_capacity_history_and_resize();
    }
    std::printf("numeric view adapter regressions: %d failures\n",failures);return failures?1:0;
}
