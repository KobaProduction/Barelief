#pragma once
#include "native_export_io.h"
namespace essentia_tempo_export {
struct Case {
    native_export::Case data;
    unsigned seed=0x52485954;
};
float noise(void*) noexcept;
void period(const Case&);
void pcm_beat(const Case&,bool multi);
void percival(const Case&);
void novelty(const Case&);
}
