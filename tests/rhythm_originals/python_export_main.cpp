#include "python_export.h"
#include "python_export_next.h"
#include "python_export_remaining.h"
#include <cstdio>
#include <exception>

int main(int argc,char** argv) {
    try {
        const auto request=originals::request(argc,argv);const auto input=originals::read_input(request);
        originals::Result result(request);bool processed=false;
        if(request.family=="spectrum")processed=originals::spectrum(request,input,result);
        else if(request.family=="librosa")processed=originals::librosa(request,input,result);
        else if(request.family=="madmom")processed=originals::madmom(request,input,result);
        else if(request.family=="models")processed=originals::models(request,input,result);
        else if(request.family=="filters")processed=originals::filters(request,input,result);
        else if(request.family=="composition")processed=originals::compositions(request,input,result);
        else if(request.family=="patterns")processed=originals::patterns(request,input,result);
        else if(request.family=="remaining")processed=originals::remaining(request,input,result);
        else originals::require(false,"unknown family");
        result.finish(true,processed);return 0;
    } catch(const std::exception& error) {std::fprintf(stderr,"originals export: %s\n",error.what());return 1;}
}
