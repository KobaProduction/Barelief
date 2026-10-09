#pragma once
#include <visualization/quick2d/plot_model.h>
#include <visualization/ui_text.h>
#include <algorithm>
#include <cmath>
#include <vector>
class NumericPieView final : public QuickPlotWidget {
public:
    explicit NumericPieView(QWidget* parent=nullptr):QuickPlotWidget(parent){setObjectName("numericPieView");refresh_presentation();}
    bool set_values(const std::vector<float>& values){
        if(values.size()>1048576){values_.clear();valid_=false;error_="Renderer projection unavailable";slices_.clear();refresh_presentation();return false;}
        values_=values;rebuild();refresh_presentation();return valid_;
    }
    bool projection_available() const {return valid_;}
    void set_projection_status(const QString& status){if(status_==status)return;status_=status;refresh_presentation();}
    size_t value_count() const {return values_.size();}
    float peak_value() const {float peak=0;for(float value:values_)if(std::isfinite(value))peak=std::max(peak,std::abs(value));return peak;}
    size_t slice_count() const {return slices_.size();}
    double slice_value(size_t index) const {return index<slices_.size()?slices_[index]:0;}
    double slice_fraction(size_t index) const {return sum_>0?slice_value(index)/sum_:0;}
protected:
    void refresh_presentation() override {
        if(!rendering_enabled_)return;
        QVariantList slices;double start=0;
        for(size_t i=0;i<slices_.size();++i){
            const double fraction=slice_fraction(i);
            slices.append(QVariantMap{{"start",start},{"fraction",fraction},{"label",i==63?ui::text("Other"):ui::text("Value %1").arg(qulonglong(i))},
                {"value",QString::number(slices_[i],'g',12)}});start+=fraction;
        }
        const QString detail=error_.isEmpty()?QString{}:ui::text(error_.toUtf8().constData());
        publish({{"kind","pie"},{"slices",slices},{"title",!valid_?detail:status_.isEmpty()?detail:status_},{"available",!slices_.empty()}});
    }
private:
    void rebuild(){
        slices_.clear();sum_=0;valid_=true;error_.clear();
        for(float value:values_){if(!std::isfinite(value)||value<0){valid_=false;error_="Pie charts require finite nonnegative values";return;}sum_+=double(value);}
        if(values_.empty())return;
        if(sum_==0){error_="All values are zero";return;}
        const size_t individual=std::min(values_.size(),size_t{63});
        for(size_t i=0;i<individual;++i)slices_.push_back(values_[i]);
        if(values_.size()>individual){double other=0;for(size_t i=individual;i<values_.size();++i)other+=double(values_[i]);slices_.push_back(other);}
    }
    std::vector<float> values_;
    std::vector<double> slices_;
    double sum_=0;
    QString status_,error_;
    bool valid_=true;
};
