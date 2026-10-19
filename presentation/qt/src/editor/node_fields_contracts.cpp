#include "node_field_builder.h"
#include "numeric_graph_draft.h"
#include <stdexcept>

namespace {
using namespace signal_contracts;
using namespace domain;
struct ContractBinding {
    PortDirection direction;PortId id;
    QString prefix() const {return QString(direction==PortDirection::Input?"input%1":"output%1").arg(id);}
    int dtype(const ConfiguredProcessingNode& node) const {
        const auto& ports=direction==PortDirection::Input?node.inputs:node.outputs;
        for(const auto& port:ports)if(port.id==id)return int(std::get<NumericContract>(port.contract.view()).dtype);
        return -1;
    }
    NumericContract read(const ConfiguredProcessingNode& node) const {
        const auto& ports=direction==PortDirection::Input?node.inputs:node.outputs;
        for(const auto& port:ports)if(port.id==id)return std::get<NumericContract>(port.contract.view());
        return {};
    }
    void modify(ConfiguredProcessingNode& node,const std::function<void(NumericContract&)>& change) const {
        DataContract data=read(node);change(std::get<NumericContract>(data));
        if(!numeric_editor::set_contract(node,direction,id,data))throw std::invalid_argument("Unsupported node settings");
    }
};
void set_contract_rank(NumericContract& contract,Rank rank) {
    contract.shape.rank=rank;
    if(rank==Rank::Scalar){contract.shape={};contract.coordinates={};contract.channel_order=contract.channel_order.first(1);return;}
    if(contract.shape.axes[0]==Axis::None)contract.shape.axes[0]=Axis::Declared;
    if(rank==Rank::Matrix){if(contract.shape.axes[1]==Axis::None)contract.shape.axes[1]=Axis::Declared;return;}
    contract.shape.axes[1]=Axis::None;contract.shape.extents[1]={1,1};contract.coordinates[1]={};
}
void set_contract_encoding(NumericContract& contract,int encoding) {
    contract.representation.encoding=StorageEncoding(encoding);
    if(encoding<3){contract.representation.valid_bits=0;return;}
    contract.dtype=DType::Int32;contract.representation.valid_bits=24;contract.representation.placement=BitPlacement::LeastSignificant;
}
void set_contract_extent(ConfiguredProcessingNode& node,ContractBinding binding,std::size_t axis,const NumericLiteral& value) {
    const auto* extent=std::get_if<std::uint64_t>(&value);if(!extent)throw std::invalid_argument("extent type");
    binding.modify(node,[axis,extent](auto& contract){contract.shape.extents[axis]={*extent,*extent};});
}
}
void NodeParameterModel::appendContractCaption(const domain::ProcessingPort& port,domain::PortDirection direction) {
    const ContractBinding binding{direction,port.id};
    auto caption=makeField("",binding.prefix()+"Caption",Editor::Caption);caption.pinnable=false;caption.section=Section::Contract;
    caption.read=[this,binding]()->Value {
        const auto& ports=binding.direction==PortDirection::Input?_node.inputs:_node.outputs;
        for(const auto& current:ports)if(current.id==binding.id)return numeric_editor::port_caption(current,!numeric_editor::is_graph_view(QString::fromStdString(_node.kind)));
        return QString{};
    };
    _fields.push_back(std::move(caption));
}
void NodeParameterModel::appendContractDtype(domain::PortDirection direction,domain::PortId id) {
    const ContractBinding binding{direction,id};QStringList names;
    for(const auto& dtype:signal_contracts::dtype_descriptors())names.append(numeric_editor::dtype_caption(dtype.type));
    choice(direction==domain::PortDirection::Input?"Input dtype":"Output dtype",binding.prefix()+"Dtype",names,
        [binding](const auto& node){return binding.dtype(node);},
        [binding](auto& node,int value){if(!numeric_editor::set_dtype(node,binding.direction,binding.id,signal_contracts::DType(value)))throw std::invalid_argument("dtype");});
}
void NodeParameterModel::appendContractShape(domain::PortDirection direction,domain::PortId id) {
    const ContractBinding binding{direction,id};
    choice("Shape",binding.prefix()+"Rank",{"Scalar","Vector","Matrix"},
        [binding](const auto& node){return int(binding.read(node).shape.rank);},
        [binding](auto& node,int rank){binding.modify(node,[rank](auto& contract){set_contract_rank(contract,signal_contracts::Rank(rank));});});
}
void NodeParameterModel::appendContractExtent(domain::PortDirection direction,domain::PortId id,std::size_t axis) {
    const ContractBinding binding{direction,id};
    literal(axis==0?"First extent":"Second extent",binding.prefix()+QString("Extent%1").arg(axis),
        [binding,axis](const auto& node){return signal_contracts::NumericLiteral{binding.read(node).shape.extents[axis].maximum};},
        [binding,axis](auto& node,const auto& value){set_contract_extent(node,binding,axis,value);},1);
    _fields.back().available=[this,binding,axis]{return std::size_t(binding.read(_node).shape.rank)>axis;};
}
void NodeParameterModel::appendContractRepresentation(domain::PortDirection direction,domain::PortId id) {
    const ContractBinding binding{direction,id};
    choice("Encoding",binding.prefix()+"Encoding",{"Native","Little endian","Big endian","Packed PCM24 LE","Packed PCM24 BE"},
        [binding](const auto& node){return int(binding.read(node).representation.encoding);},
        [binding](auto& node,int encoding){binding.modify(node,[encoding](auto& contract){set_contract_encoding(contract,encoding);});});
    choice("Normalization",binding.prefix()+"Normalization",{"None","Signed unit","Unsigned unit","Float unit"},
        [binding](const auto& node){return int(binding.read(node).representation.normalization);},
        [binding](auto& node,int value){binding.modify(node,[value](auto& contract){contract.representation.normalization=signal_contracts::Normalization(value);});});
}
void NodeParameterModel::appendContractPort(const domain::ProcessingPort& port,domain::PortDirection direction) {
    appendContractCaption(port,direction);
    if(numeric_editor::is_graph_view(QString::fromStdString(_node.kind)) || !std::holds_alternative<signal_contracts::NumericContract>(port.contract.view()))return;
    appendContractDtype(direction,port.id);appendContractShape(direction,port.id);
    for(std::size_t axis=0;axis<2;++axis)appendContractExtent(direction,port.id,axis);
    appendContractRepresentation(direction,port.id);
}
void NodeParameterModel::append_contracts_fields() {
    _field_section=Section::Contract;
    for(auto direction:{domain::PortDirection::Input,domain::PortDirection::Output}){
        const auto& ports=direction==domain::PortDirection::Input?_node.inputs:_node.outputs;
        for(const auto& port:ports)appendContractPort(port,direction);
    }
    for(auto& field:_fields)if(field.section==Section::Contract){field.pinnable=false;field.hide_inline=true;}
}
