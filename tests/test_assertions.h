#pragma once

#include <cstdio>
#include <cstdlib>

namespace test_assertions {
inline void record(bool passed, const char* expression, const char* file,
                   int line, int& failures) noexcept {
    if (passed) return;
    std::printf("FAIL %s:%d: %s\n", file, line, expression);
    ++failures;
}
inline bool verify(bool passed,const char* expression,const char* file,int line) noexcept {
    if(!passed)std::fprintf(stderr,"FAIL %s:%d: %s\n",file,line,expression);
    return passed;
}
inline void require(bool passed,const char* expression,const char* file,int line) noexcept {
    if(!verify(passed,expression,file,line))std::abort();
}
}
