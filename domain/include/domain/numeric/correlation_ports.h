#pragma once
#include <domain/graph/processing_model.h>

namespace domain {
enum class CorrelationInput:PortId {Reference=processing_input,Compared=1,LagCount=window_size_input};
enum class CorrelationOutput:PortId {Values=processing_output};
}
