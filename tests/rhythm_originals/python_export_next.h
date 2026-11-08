#pragma once
#include "python_export.h"

namespace originals {
// New finite packet. Existing driver dispatch is wired only at source fan-in.
bool compositions(const Request&,const Matrix&,Result&);
bool patterns(const Request&,const Matrix&,Result&);
Matrix sidecar(const Request&,std::string_view,std::size_t,std::size_t);
}
