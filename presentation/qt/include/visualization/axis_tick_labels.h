#pragma once
#include <QLocale>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

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
    QStringList labels;
    for (int precision = std::clamp(minimumPrecision, 0, 15); precision <= 17; ++precision) {
        labels.clear();
        bool accurate = true;
        for (size_t i = 0; i < ticks.size(); ++i) {
            const double value = ticks[i];
            if (!std::isfinite(value)) { labels << QString{}; continue; }
            auto label = locale.toString(value == 0 ? 0.0 : value, format, precision);
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
