#pragma once
#include <domain/graph/processing_model.h>

namespace domain {
enum class ArrayInput:PortId {Values=0};
enum class ArrayOutput:PortId {Values=0};
enum class FourierInput:PortId {Real=0,Imaginary=1};
enum class FourierOutput:PortId {Real=0,Imaginary=1};
enum class PolarOutput:PortId {Magnitude=0,Phase=1};
enum class ArrayAxis {First,Second,Last};
enum class RecursiveFilterInput:PortId {Values=0,InitialState=1};
enum class RecursiveFilterOutput:PortId {Values=0,FinalState=1};
enum class FilterStatePolicy {Continue,ResetBatch,ResetRows};
enum class ArrayPadding {Constant,Edge};
}
