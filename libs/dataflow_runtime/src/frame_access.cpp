#include "frame_access.h"

namespace dataflow_runtime::detail {
MutableNumericView mutable_output(const PlanNode& n,State& state,PortId p) noexcept {auto& b=state.numeric_outputs[p];
        MutableNumericView out{&std::get<NumericContract>(n.numeric_outputs[p]),b.storage(),b.planes,b.actual,{},{}};
        for(std::size_t axis=0;axis<2;++axis)if(!b.coordinates[axis].empty())out.coordinates.axes[axis]={b.coordinates[axis].data(),b.actual[axis]*8};
        return out;}
std::variant<NumericView,StructuredView> zero_value(const DataContract& contract,const NumericBuffer& zero) noexcept {
        if(const auto* numeric=std::get_if<NumericContract>(&contract)){
            auto value=NumericView{numeric,zero.storage(),zero.planes,zero.actual,{},{},true};
            for(std::size_t axis=0;axis<2;++axis)if(!zero.coordinates[axis].empty())value.coordinates.axes[axis]={zero.coordinates[axis].data(),zero.actual[axis]*8};
            return value;
        }
        return StructuredView{&std::get<StructuredContract>(contract),zero.storage()};
    }
}
