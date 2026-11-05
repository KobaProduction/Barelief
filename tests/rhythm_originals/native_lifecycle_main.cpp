#include "native_lifecycle_io.h"
#include <cstdio>

int main(int argc,char** argv) {
    try {
        native_lifecycle::require(argc==4,"usage: lifecycle plan input.f32 output-prefix");
        const auto plan=native_lifecycle::read_plan(argv[1]);const auto pcm=native_lifecycle::read_pcm(argv[2]);
        auto operation=plan.component<=native_lifecycle::Component::Tempo?native_lifecycle::aubio(plan):native_lifecycle::btrack(plan);
        native_lifecycle::require(bool(operation),"lifecycle operation construction");
        native_lifecycle::Output output(argv[3]);native_lifecycle::execute(plan,pcm,*operation,output);return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"%s\n",error.what());return 1;}
}
