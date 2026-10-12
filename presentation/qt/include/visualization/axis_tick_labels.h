#pragma once
#include <QLocale>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace axis_tick_detail {
// Decimal half ties can straddle either side after subtraction/interpolation of
// physical coordinates. Stabilize only their *display rounding*, within a small
// floating-point error budget. Never quantize source coordinates or a resolvable
// division: when requested decimal precision approaches that budget, use the
// original value and let the ordinary uniqueness check increase precision.
inline double stableRoundingValue(double value, char format, int precision, double errorBudget) {
    if(!value || !std::isfinite(value) || !std::isfinite(errorBudget))return value;
    const int exponent=format=='e'?int(std::floor(std::log10(std::abs(value)))):0;
    const long double quantum=std::pow(10.L,exponent-precision);
    if(!std::isfinite(quantum) || quantum<=0 || errorBudget>quantum*1e-6L)return value;
    const long double scaled=std::abs(static_cast<long double>(value))/quantum;
    const long double lower=std::floor(scaled),half=lower+.5L;
    if(std::abs(scaled-half)*quantum>errorBudget)return value;
    // Deterministic half-away-from-zero, then format the rounded decimal value
    // through QLocale. This result is a full half-quantum away from the tie.
    const double rounded=std::copysign(double((lower+1)*quantum),value);
    return std::isfinite(rounded)?rounded:value;
}
}

// Format the actual tick coordinates, including nonuniform metadata coordinates.
// Requested precision is a minimum: it must not collapse distinct grid divisions.
inline QStringList axisTickLabels(const std::vector<double> &ticks, QLocale locale,
                                 int minimumPrecision = 0, QString *printfFormat = nullptr) {
    locale.setNumberOptions(locale.numberOptions() | QLocale::OmitGroupSeparator);
    double step = std::numeric_limits<double>::infinity(), magnitude = 0;
    for (size_t i = 0; i < ticks.size(); ++i) {
        if (!std::isfinite(ticks[i])) continue;
        magnitude = std::max(magnitude, std::abs(ticks[i]));
        if (i && std::isfinite(ticks[i-1]) && ticks[i] != ticks[i-1])
            step = std::min(step, std::abs(ticks[i] - ticks[i-1]));
    }
    const char format = magnitude >= 1e7 || step < 1e-5 ? 'e' : 'f';
    const double errorBudget=32*std::numeric_limits<double>::epsilon()*magnitude;
    QStringList labels;
    for (int precision = std::clamp(minimumPrecision, 0, 15); precision <= 17; ++precision) {
        labels.clear();
        bool accurate = true;
        for (size_t i = 0; i < ticks.size(); ++i) {
            const double value = ticks[i];
            if (!std::isfinite(value)) { labels << QString{}; continue; }
            const double display=axis_tick_detail::stableRoundingValue(value,format,precision,errorBudget);
            auto label = locale.toString(display == 0 ? 0.0 : display, format, precision);
            double rounded = locale.toDouble(label);
            if (rounded == 0) label = locale.toString(0.0, format, precision);
            labels << label;
            // One percent of a division is ample for readability without printing
            // float interpolation noise. Check uniqueness as well for large offsets.
            if (std::isfinite(step) && std::abs(rounded-value) > step*.01) accurate = false;
            if (i && value != ticks[i-1] && std::isfinite(ticks[i-1]) && label == labels[int(i)-1])
                accurate = false;
        }
        if (accurate || precision==17) {
            if(printfFormat) *printfFormat=QString("%.")+QString::number(precision)+QChar(format);
            break;
        }
    }
    return labels;
}


// A readout has no neighbouring division from which to infer precision. Use its
// distance from zero as a division, so nonzero tiny samples cannot become zero.
inline QString axisReadoutLabel(double value, QLocale locale, int minimumPrecision=0) {
    if(!std::isfinite(value))return {};
    return axisTickLabels({0,value},locale,minimumPrecision).back();
}
