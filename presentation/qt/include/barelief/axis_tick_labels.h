#pragma once
#include <QLocale>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>
#include <utility>

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
struct TickGeometry {
    double step=std::numeric_limits<double>::infinity();
    double magnitude=0;
};
inline TickGeometry tickGeometry(const std::vector<double>& ticks) {
    TickGeometry result;
    for(size_t i=0;i<ticks.size();++i) {
        if(!std::isfinite(ticks[i]))continue;
        result.magnitude=std::max(result.magnitude,std::abs(ticks[i]));
        if(i && std::isfinite(ticks[i-1]) && ticks[i]!=ticks[i-1])result.step=std::min(result.step,std::abs(ticks[i]-ticks[i-1]));
    }
    return result;
}
class TickLabelFormatter {
public:
    TickLabelFormatter(const std::vector<double>& ticks,QLocale locale):ticks_(ticks),locale_(std::move(locale)),geometry_(tickGeometry(ticks)) {
        locale_.setNumberOptions(locale_.numberOptions() | QLocale::OmitGroupSeparator);
    }
    char format() const {return geometry_.magnitude>=1e7 || geometry_.step<1e-5?'e':'f';}
    QStringList labels(int precision,bool& accurate) const {
        QStringList result;
        accurate=true;
        for(size_t i=0;i<ticks_.size();++i) {
            if(!std::isfinite(ticks_[i])){result << QString{};continue;}
            const auto label=formattedValue(ticks_[i],precision);
            if(!resolvesDivision(i,label,result))accurate=false;
            result << label;
        }
        return result;
    }
private:
    QString formattedValue(double value,int precision) const {
        const double errorBudget=32*std::numeric_limits<double>::epsilon()*geometry_.magnitude;
        const double display=stableRoundingValue(value,format(),precision,errorBudget);
        auto label=locale_.toString(display==0?0.0:display,format(),precision);
        if(locale_.toDouble(label)==0)label=locale_.toString(0.0,format(),precision);
        return label;
    }
    bool resolvesDivision(size_t index,const QString& label,const QStringList& previous) const {
        // One percent of a division avoids interpolation noise; uniqueness also
        // protects grids at large offsets where relative error is insufficient.
        if(std::isfinite(geometry_.step) && std::abs(locale_.toDouble(label)-ticks_[index])>geometry_.step*.01)return false;
        return !index || ticks_[index]==ticks_[index-1] || !std::isfinite(ticks_[index-1]) || label!=previous[int(index)-1];
    }
    const std::vector<double>& ticks_;
    QLocale locale_;
    TickGeometry geometry_;
};
}

// Format the actual tick coordinates, including nonuniform metadata coordinates.
// Requested precision is a minimum: it must not collapse distinct grid divisions.
inline QStringList axisTickLabels(const std::vector<double> &ticks, QLocale locale,
                                 int minimumPrecision = 0, QString *printfFormat = nullptr) {
    const axis_tick_detail::TickLabelFormatter formatter(ticks,std::move(locale));
    QStringList labels;
    for (int precision = std::clamp(minimumPrecision, 0, 15); precision <= 17; ++precision) {
        bool accurate=false;
        labels=formatter.labels(precision,accurate);
        if (accurate || precision==17) {
            if(printfFormat) *printfFormat=QString("%.")+QString::number(precision)+QChar(formatter.format());
            break;
        }
    }
    return labels;
}


// A readout has no neighbouring division from which to infer precision. Use its
// distance from zero as a division, so nonzero tiny samples cannot become zero.
inline QString axisReadoutLabel(double value, QLocale locale, int minimumPrecision=0) {
    if(!std::isfinite(value))return {};
    const auto labels=axisTickLabels({0,value},locale,minimumPrecision);
    return labels.back();
}
