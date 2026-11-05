#include "python_export.h"
#include <algorithm>
#include <bit>
#include <fstream>
#include <iomanip>
#include <limits>
#include <stdexcept>

namespace originals {
void require(bool value,std::string_view message) {if(!value)throw std::runtime_error(std::string(message));}
void report_workspace(Result& result,std::size_t bytes) {result.integers("workspace_bytes",std::span(&bytes,1));}
double Request::number(std::size_t index,double fallback) const {return index<arguments.size()?std::stod(arguments[index]):fallback;}
std::size_t Request::size(std::size_t index,std::size_t fallback) const {return index<arguments.size()?std::stoull(arguments[index]):fallback;}
Request request(int argc,char** argv) {
    require(argc>=7,"family method input prefix rows columns [settings]");
    Request result{argv[1],argv[2],argv[3],argv[4],std::stoull(argv[5]),std::stoull(argv[6]),{}};
    require(result.frames<=1048576 && result.columns && result.columns<=65536,"fixture geometry");
    require(result.frames<=16777216/result.columns,"fixture element bound");
    for(int i=7;i<argc;++i)result.arguments.emplace_back(argv[i]);
    return result;
}
Matrix read_input(const Request& request) {
    static_assert(sizeof(float)==4 && std::endian::native==std::endian::little);
    Matrix result(request.frames,request.columns);
    std::ifstream file(request.input,std::ios::binary|std::ios::ate);
    require(bool(file) && file.tellg()==std::streamoff(result.values.size()*sizeof(float)),"exact Float32 input bytes");
    file.seekg(0);file.read(reinterpret_cast<char*>(result.values.data()),std::streamsize(result.values.size()*sizeof(float)));
    require(bool(file),"read input");return result;
}
bool Matrix::read(const void* context,std::size_t row,std::span<float> output) noexcept {
    const auto& self=*static_cast<const Matrix*>(context);
    if(output.size()!=self.columns || row>=self.values.size()/self.columns)return false;
    std::copy_n(self.values.begin()+std::ptrdiff_t(row*self.columns),self.columns,output.begin());return true;
}
bool Matrix::write(void* context,std::size_t row,std::span<const float> input) noexcept {
    auto& self=*static_cast<Matrix*>(context);
    if(input.size()!=self.columns || row>=self.values.size()/self.columns)return false;
    std::copy(input.begin(),input.end(),self.values.begin()+std::ptrdiff_t(row*self.columns));return true;
}
void Result::bytes(std::string_view name,std::string dtype,std::span<const std::byte> values,std::vector<std::size_t> shape) {
    std::ofstream file(prefix_.string()+"."+std::string(name)+".bin",std::ios::binary);
    file.write(reinterpret_cast<const char*>(values.data()),std::streamsize(values.size()));
    require(bool(file),"write binary output");outputs_.push_back({std::string(name),std::move(dtype),std::move(shape)});
}
void Result::floats(std::string_view name,std::span<const float> values,std::vector<std::size_t> shape) {bytes(name,"float32-le",std::as_bytes(values),std::move(shape));}
void Result::doubles(std::string_view name,std::span<const double> values,std::vector<std::size_t> shape) {bytes(name,"float64-le",std::as_bytes(values),std::move(shape));}
void Result::integers(std::string_view name,std::span<const std::size_t> values) {
    std::vector<std::uint64_t> converted(values.begin(),values.end());bytes(name,"uint64-le",std::as_bytes(std::span(converted)),{values.size()});
}
void Result::states(std::string_view name,std::span<const std::uint32_t> values) {bytes(name,"uint32-le",std::as_bytes(values),{values.size()});}
void Result::signed_integers(std::string_view name,std::span<const std::int32_t> values) {bytes(name,"int32-le",std::as_bytes(values),{values.size()});}
void Result::mask(std::string_view name,std::span<const std::uint8_t> values) {bytes(name,"uint8",std::as_bytes(values),{values.size()});}
namespace {
void write_output(std::ostream& file,const Output& value) {
    file<<"{\"name\":"<<std::quoted(value.name)<<",\"dtype\":"<<std::quoted(value.dtype)<<",\"shape\":[";
    for(std::size_t i=0;i<value.shape.size();++i){if(i)file<<',';file<<value.shape[i];}
    file<<"]}";
}
}
void Result::finish(bool prepared,bool processed) const {
    std::ofstream file(prefix_.string()+".json");
    file<<"{\"prepared\":"<<(prepared?"true":"false")<<",\"processed\":"<<(processed?"true":"false")<<",\"outputs\":[";
    for(std::size_t i=0;i<outputs_.size();++i){if(i)file<<',';write_output(file,outputs_[i]);}
    file<<"]}\n";require(bool(file),"write result manifest");
}
}
