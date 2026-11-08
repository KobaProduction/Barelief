#include "numeric_operation_requirements.h"
#include <algorithm>
#include <cmath>

namespace dataflow_runtime::detail {
using namespace signal_contracts;
bool metadata_close(double actual,double expected,double tolerance) noexcept {
    return std::isfinite(actual) && std::isfinite(expected) && std::abs(actual-expected)<=tolerance*std::max(1.,std::abs(expected));
}
bool metadata_index_unit(NumericUnit unit) noexcept {return unit==NumericUnit::Values || unit==NumericUnit::Frames;}
bool metadata_axis_ignored(const AxisCoordinates& axis,const NumericOperationAxisRule& rule) noexcept {
    return rule.ignore_other_units && axis.unit!=rule.unit && !(rule.allow_index && metadata_index_unit(axis.unit));
}
bool metadata_grid(const AxisCoordinates& axis,const NumericOperationAxisRule& rule,NumericMetadataGrid& grid) noexcept {
    const bool index=rule.kind==NumericOperationAxisKind::Index || (rule.allow_index && metadata_index_unit(axis.unit));
    grid.tolerance=rule.tolerance;grid.any_origin=!index && rule.any_origin;
    grid.origin=index?0:rule.exact_origin?double(rule.exact_origin->numerator)/double(rule.exact_origin->denominator):rule.origin;
    grid.step=index?1:rule.exact_step?double(rule.exact_step->numerator)/double(rule.exact_step->denominator):rule.step;
    return std::isfinite(grid.origin) && std::isfinite(grid.step) && std::isfinite(grid.tolerance) && grid.tolerance>=0;
}
namespace {
bool valid_axis_rule(const NumericOperationAxisRule& rule) noexcept {
    const bool kind=rule.kind==NumericOperationAxisKind::Index || rule.kind==NumericOperationAxisKind::Uniform || rule.kind==NumericOperationAxisKind::Declaration;
    return kind && static_cast<unsigned>(rule.unit)<=static_cast<unsigned>(NumericUnit::Decibels) &&
        std::isfinite(rule.tolerance) && rule.tolerance>=0 && std::isfinite(rule.origin) && std::isfinite(rule.step) &&
        (!rule.exact_origin || rule.exact_origin->denominator) && (!rule.exact_step || rule.exact_step->denominator);
}
bool index_declaration(const AxisCoordinates& axis) noexcept {
    return axis.mode==CoordinateMode::Regular && metadata_index_unit(axis.unit) && !axis.origin.numerator &&
        axis.origin.denominator && axis.step.numerator>0 && std::uint64_t(axis.step.numerator)==axis.step.denominator;
}
bool grid_origin(const AxisCoordinates& axis,const NumericOperationAxisRule& rule,const NumericMetadataGrid& grid) noexcept {
    if(rule.exact_origin)return axis.origin==*rule.exact_origin;
    if(grid.any_origin)return true;
    if(grid.origin==0)return !axis.origin.numerator;
    return metadata_close(double(axis.origin.numerator)/double(axis.origin.denominator),grid.origin,grid.tolerance);
}
bool grid_step(const AxisCoordinates& axis,const NumericMetadataGrid& grid) noexcept {
    if(grid.step>0 && axis.step.numerator<=0)return false;
    if(grid.step<0 && axis.step.numerator>=0)return false;
    return metadata_close(double(axis.step.numerator)/double(axis.step.denominator),grid.step,grid.tolerance);
}
bool grid_declaration(const AxisCoordinates& axis,const NumericOperationAxisRule& rule) noexcept {
    if(rule.exact_step && axis.step!=*rule.exact_step)return false;
    if(rule.kind==NumericOperationAxisKind::Index || (rule.allow_index && metadata_index_unit(axis.unit)))return index_declaration(axis);
    NumericMetadataGrid grid;if(!metadata_grid(axis,rule,grid) || !axis.origin.denominator || !axis.step.denominator)return false;
    return grid_origin(axis,rule,grid) && grid_step(axis,grid);
}
bool declaration(const AxisCoordinates& axis,const NumericOperationAxisRule& rule) noexcept {
    if(!rule.mode)return rule.ignore_other_units || axis.unit==rule.unit;
    if(*rule.mode==CoordinateMode::Regular)return grid_declaration(axis,rule);
    AxisCoordinates expected{*rule.mode,rule.unit};
    if(rule.exact_origin)expected.origin=*rule.exact_origin;
    if(rule.exact_step)expected.step=*rule.exact_step;
    return axis==expected;
}
bool defined_axis(const AxisCoordinates& axis,const NumericOperationAxisRule& rule) noexcept {
    if(rule.kind==NumericOperationAxisKind::Declaration)return declaration(axis,rule);
    if(rule.kind!=NumericOperationAxisKind::Uniform && rule.kind!=NumericOperationAxisKind::Index)return false;
    const bool index=metadata_index_unit(axis.unit) && (rule.allow_index || rule.kind==NumericOperationAxisKind::Index);
    if(!index && axis.unit!=rule.unit)return false;
    if(axis.mode==CoordinateMode::ExplicitFloat64LE)return rule.allow_explicit;
    return grid_declaration(axis,rule);
}
bool axis_declaration_tags(const NumericContract& contract,const NumericOperationAxisRule& rule) noexcept {
    const auto& axis=contract.coordinates[rule.axis];
    if(rule.tag && contract.shape.axes[rule.axis]!=*rule.tag)return false;
    if(rule.mode && axis.mode!=*rule.mode)return false;
    return axis.mode!=CoordinateMode::ExplicitFloat64LE || rule.allow_explicit;
}
}
bool metadata_axis_valid(const NumericContract& contract,const NumericOperationAxisRule& rule) noexcept {
    if(rule.axis>=2 || !valid_axis_rule(rule))return false;
    const auto& axis=contract.coordinates[rule.axis];
    if(!axis_declaration_tags(contract,rule))return false;
    if(axis.mode==CoordinateMode::None)return rule.allow_absent && (!rule.default_absent || axis==AxisCoordinates{});
    if(metadata_axis_ignored(axis,rule))return true;
    if(rule.require_increasing && axis.mode==CoordinateMode::Regular && axis.step.numerator<=0)return false;
    return defined_axis(axis,rule);
}
}
