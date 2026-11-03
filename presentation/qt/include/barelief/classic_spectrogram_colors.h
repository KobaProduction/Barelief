#pragma once
#include <QColor>
#include <QGradient>
#include <array>
#include <algorithm>
#include <cmath>

// Data colors, deliberately independent of the interface theme.
inline QGradientStops classicSpectrogramStops() {
    return {{0,QColor(0x00,0x00,0x04)},{.125,QColor(0x1c,0x10,0x44)},{.25,QColor(0x51,0x12,0x7c)},
        {.375,QColor(0x83,0x26,0x81)},{.5,QColor(0xb7,0x37,0x79)},{.625,QColor(0xe7,0x52,0x63)},
        {.75,QColor(0xfc,0x89,0x61)},{.875,QColor(0xfe,0xc2,0x87)},{1,QColor(0xfc,0xfd,0xbf)}};
}
inline std::array<QRgb,256> classicSpectrogramColors() {
    const auto stops=classicSpectrogramStops();std::array<QRgb,256> colors{};
    for(size_t i=0;i<colors.size();++i) {
        const double position=double(i)*8/255;const int stop=std::min(7,int(position));
        const auto a=stops[stop].second,b=stops[stop+1].second;const double f=position-stop;
        colors[i]=qRgb(qRound(a.red()+(b.red()-a.red())*f),qRound(a.green()+(b.green()-a.green())*f),qRound(a.blue()+(b.blue()-a.blue())*f));
    }
    return colors;
}
