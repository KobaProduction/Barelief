#pragma once
#include <cstdint>
#include <fstream>
#include <initializer_list>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace native_export {
struct Case {
    std::uint32_t frame=1024,hop=512,rate=44100;
    unsigned window=1;
    std::vector<float> pcm;
    std::string prefix;
};
inline void require(bool success,const char* operation) {
    if(!success)throw std::runtime_error(operation);
}
class Output {
public:
    explicit Output(std::string prefix):prefix_(std::move(prefix)) {}
    template<class T> void row(const std::string& name,std::span<const T> values) {
        auto [it,inserted]=streams_.try_emplace(name);
        if(inserted)it->second.open(prefix_+"."+name,std::ios::binary);
        it->second.write(reinterpret_cast<const char*>(values.data()),std::streamsize(values.size_bytes()));
        require(bool(it->second),"binary output write");
    }
    template<class T> void values(const std::string& name,std::initializer_list<T> values) {
        row<T>(name,{values.begin(),values.size()});
    }
private:
    std::string prefix_;
    std::map<std::string,std::ofstream> streams_;
};
void aubio(const Case&);
void aubio_features(const Case&);
void btrack(const Case&);
void essentia(const Case&);
}
