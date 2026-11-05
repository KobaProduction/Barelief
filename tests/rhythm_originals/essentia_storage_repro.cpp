// SPDX-License-Identifier: AGPL-3.0-or-later
// Minimal reproduction of MTG/essentia@b9fa6cb6 onsetdetectionglobal.cpp:410-415.
// This standalone TU must never link a release Essentia library with _GLIBCXX_DEBUG.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
std::vector<float> weights(const std::string& pattern) {
    std::vector<float> result(40,1.f);
    if(pattern=="descending")for(std::size_t i=0;i<result.size();++i)result[i]=float(40-i);
    else if(pattern!="ties")throw std::runtime_error("unknown weight pattern");
    return result;
}
float threshold(const std::vector<float>& values,bool sized) {
    std::vector<float> sorted;
    if(sized)sorted.resize(40);else sorted.reserve(40);
    std::printf("STORAGE logical_size=%zu capacity=%zu threshold_index=24\n",sorted.size(),sorted.capacity());
    std::fflush(stdout);
    std::copy(values.begin(),values.end(),sorted.begin());
    std::sort(sorted.begin(),sorted.end());
    return sorted[int(std::floor(40*.6))];
}
int reproduce(const std::string& mode,const std::string& pattern) {
    if(mode!="raw" && mode!="sized")throw std::runtime_error("unknown storage mode");
    const auto values=weights(pattern);const auto cutoff=threshold(values,mode=="sized");
    const auto selected=std::count_if(values.begin(),values.end(),[=](float value){return value>=cutoff;});
    std::printf("THRESHOLD value=%.9g selected=%lld bands=40\n",double(cutoff),static_cast<long long>(selected));
    const bool valid=pattern=="descending"?cutoff==25.f && selected==16:cutoff==1.f && selected==40;
    return valid?0:1;
}
}
int main(int argc,char** argv) {
    try {
        if(argc!=3)throw std::runtime_error("expected raw|sized descending|ties");
        return reproduce(argv[1],argv[2]);
    }catch(const std::exception& error){std::fprintf(stderr,"PROBE_SETUP_ERROR: %s\n",error.what());return 64;}
}
