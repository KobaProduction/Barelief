#pragma once
#include <domain/graph/processing_model.h>

namespace domain::atomic_ports {
namespace comb {inline constexpr PortId values=0,responses=0,delays=1;}
namespace maximum {inline constexpr PortId values=0,indices=0,maximum=1,tied_values=2;}
namespace sum {inline constexpr PortId values=0,result=0;}
namespace gather {inline constexpr PortId table=0,indices=1,values=0;}
namespace convolution {inline constexpr PortId values=0,kernel=1,result=0;}
namespace peaks {inline constexpr PortId values=0,indices=0,heights=1;}
namespace decoder {inline constexpr PortId activations=0,intervals=1,times=0,positions=1,trials=2,scores=3;}
}
