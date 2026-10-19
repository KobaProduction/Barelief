#pragma once
#include <QString>
#include <visualization/value_display_scale.h>
#include <vector>
#include <cmath>
#include <limits>
#include <algorithm>
// Renderer coordinates are a bounded projection. Geometry remains indexed;
// labels/readouts map those indices to declared coordinates without changing data.
struct SpectrogramAxisMetadata {
    QString time_title_key=QStringLiteral("Row index (X)");
    QString column_title_key=QStringLiteral("Column index (Z)");
    QString value_title_key=QStringLiteral("Value (Y)");
    QString column_index_title_key=QStringLiteral("Column index (Z)");
    QString time_index_title_key=QStringLiteral("Row index (X)");
    bool time_coordinates=false,column_coordinates=false;
    bool age_axis=true;
    std::vector<double> ages,columns,row_indices;
    ValueDisplayScale value_scale;
    bool operator==(const SpectrogramAxisMetadata&) const=default;
    static double coordinate(const std::vector<double>& values,double index) noexcept {
        if(!std::isfinite(index) || index<0 || values.empty() || index>double(values.size()-1))return std::numeric_limits<double>::quiet_NaN();
        const auto lower=std::size_t(index),upper=std::min(lower+1,values.size()-1);
        return values[lower]+(values[upper]-values[lower])*(index-double(lower));
    }
    double age(double index) const noexcept {return time_coordinates || (!age_axis && !ages.empty())?coordinate(ages,index):index;}
    double column(double index) const noexcept {return column_coordinates?coordinate(columns,index):index;}
    SpectrogramAxisMetadata displayed(const QString& frequencyMode,const QString& timeMode) const {
        auto result=*this;
        if(result.value_scale.decibels())result.value_title_key="Level, dB (Y)";
        else if(result.value_scale.source_decibels)result.value_title_key="Linear value (Y)";
        if(frequencyMode=="positions") {
            result.column_coordinates=false;result.columns.clear();result.column_title_key=column_index_title_key;
        }
        if(timeMode=="positions") {
            result.ages=row_indices;result.time_coordinates=!row_indices.empty();result.time_title_key=time_index_title_key;
        }
        return result;
    }
};
