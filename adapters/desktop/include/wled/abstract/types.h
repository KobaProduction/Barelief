#ifndef WLED_ABSTRACT_TYPES_H
#define WLED_ABSTRACT_TYPES_H

#include <cstdint>

namespace wled_audio_reactive {
    static constexpr uint16_t DEFAULT_PORT = 11988;

    // WLED audio sync packet v2 (44 bytes total)
    // Based on: https://github.com/MoonModules/WLED-MM/blob/mdev/usermods/audioreactive/audio_reactive.h

    struct __attribute__((packed)) AudioSyncPacket {
        char header[6];              // 6 bytes, offset 0 - "00002\0" for protocol version 2
        uint8_t pressure[2];         // 2 bytes, offset 6 - sound pressure as fixed point (8bit int, 8bit frac)
        float sampleRaw;             // 4 bytes, offset 8 - raw sample
        float sampleSmth;            // 4 bytes, offset 12 - smoothed sample
        uint8_t samplePeak;          // 1 byte,  offset 16 - peak detection flag
        uint8_t frameCounter;        // 1 byte,  offset 17 - rolling counter for duplicate detection
        uint8_t fftResult[16];       // 16 bytes, offset 18 - 16 GEQ channels (one byte each)
        uint16_t zeroCrossingCount;  // 2 bytes, offset 34 - zero crossing count
        float FFT_Magnitude;         // 4 bytes, offset 36 - largest FFT magnitude
        float FFT_MajorPeak;         // 4 bytes, offset 40 - frequency of largest FFT peak (Hz)
    };

    static_assert(sizeof(AudioSyncPacket) == 44, "AudioSyncPacket must be exactly 44 bytes");
}

#endif // WLED_ABSTRACT_TYPES_H
