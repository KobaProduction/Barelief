#include <rhythm_reference_ports/onset_descriptor.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <new>
#include <span>
namespace {
std::size_t allocations=0;
void check(bool value,const char* why) {if(!value){std::fprintf(stderr,"FAIL Aubio phase source: %s\n",why);std::exit(1);}}
void near(float value,double expected,const char* why) {
    if(std::abs(double(value)-expected)>2e-5*std::max(1.,std::abs(expected))){
        std::fprintf(stderr,"got %.9g, expected %.12g\n",double(value),expected);check(false,why);
    }
}
std::array<float,3*2*513> phase_source_fixture(const char* name) {
    std::array<float,3*2*513> values{};
    std::ifstream input(std::filesystem::path(__FILE__).parent_path()/"fixtures"/"rhythm_originals"/name,std::ios::binary);
    input.read(reinterpret_cast<char*>(values.data()),sizeof(values));
    check(bool(input),"read exact pinned Aubio Float32 polar fixture");return values;
}
void histogram_phase_source_case(const char* name,std::array<float,2> expected) {
    namespace r=rhythm_reference_ports;
    const auto input=phase_source_fixture(name);
    constexpr std::array methods{r::OnsetRecipe::Phase,r::OnsetRecipe::WeightedPhase};
    for(std::size_t method=0;method<methods.size();++method){
        r::PreparedOnsetDescriptor kernel;check(kernel.prepare(513,methods[method]),"prepare source phase recipe");
        float output=0;const auto before=allocations;
        for(std::size_t row=0;row<3;++row){
            const auto values=std::span(input).subspan(row*2*513,2*513);
            check(kernel.process(values.first(513),values.last(513),output),"process exact source phase history");
        }
        check(allocations==before,"source phase processing never allocates");
        near(output,expected[method],"phase histogram matches actual pinned source on identical Float32 polar data");
    }
}
void histogram_phase_source_regression() {
    // Actual aubio_specdesc_do outputs at ad5cf975; three rows retain exactly
    // the two preceding phase rows. Provenance and hashes accompany fixtures.
    histogram_phase_source_case("aubio-phase-frame43.f32",{80.00065612792969f,77.97360229492188f});
    histogram_phase_source_case("aubio-phase-frame106.f32",{.3324242830276489f,5.616265296936035f});
}
}
void* operator new(std::size_t bytes) {++allocations;if(auto* value=std::malloc(bytes?bytes:1))return value;throw std::bad_alloc{};}
void* operator new[](std::size_t bytes) {return ::operator new(bytes);}
void operator delete(void* value) noexcept {std::free(value);}
void operator delete[](void* value) noexcept {::operator delete(value);}
void operator delete(void* value,std::size_t) noexcept {::operator delete(value);}
void operator delete[](void* value,std::size_t) noexcept {::operator delete(value);}
int main() {histogram_phase_source_regression();std::puts("Aubio phase source fixtures PASS");}
