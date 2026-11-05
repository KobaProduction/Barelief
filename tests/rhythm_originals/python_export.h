#pragma once
#include <dsp_kernels/numeric_rows.h>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>
namespace dsp_kernels {struct SpectralFrameRecipeConfig;}

namespace originals {
struct Request {
    std::string family,method;
    std::filesystem::path input,prefix;
    std::size_t frames=0,columns=0;
    std::vector<std::string> arguments;
    double number(std::size_t,double fallback=0) const;
    std::size_t size(std::size_t,std::size_t fallback=0) const;
};
struct Matrix {
    std::vector<float> values;
    std::size_t columns=0;
    Matrix(std::size_t rows,std::size_t width):values(rows*width),columns(width){}
    static bool read(const void*,std::size_t,std::span<float>) noexcept;
    static bool write(void*,std::size_t,std::span<const float>) noexcept;
    dsp_kernels::FloatMatrixSource source() const noexcept {return {this,read};}
    dsp_kernels::FloatMatrixDestination destination() noexcept {return {this,write};}
};
struct Output {
    std::string name,dtype;
    std::vector<std::size_t> shape;
};
class Result {
public:
    explicit Result(const Request& request):prefix_(request.prefix){}
    void floats(std::string_view,std::span<const float>,std::vector<std::size_t>);
    void doubles(std::string_view,std::span<const double>,std::vector<std::size_t>);
    void integers(std::string_view,std::span<const std::size_t>);
    void states(std::string_view,std::span<const std::uint32_t>);
    void signed_integers(std::string_view,std::span<const std::int32_t>);
    void mask(std::string_view,std::span<const std::uint8_t>);
    void finish(bool prepared,bool processed) const;
private:
    void bytes(std::string_view,std::string,std::span<const std::byte>,std::vector<std::size_t>);
    std::filesystem::path prefix_;
    std::vector<Output> outputs_;
};
Request request(int,char**);
Matrix read_input(const Request&);
bool spectrum(const Request&,const Matrix&,Result&);
bool librosa(const Request&,const Matrix&,Result&);
bool madmom(const Request&,const Matrix&,Result&);
bool models(const Request&,const Matrix&,Result&);
bool filters(const Request&,const Matrix&,Result&);
Matrix spectral_stages(std::span<const float>,const dsp_kernels::SpectralFrameRecipeConfig&,Result&);
void require(bool,std::string_view);
void report_workspace(Result&,std::size_t);
}
