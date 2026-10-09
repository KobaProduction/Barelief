#pragma once
#include <visualization/ui_text.h>
#include <QChartView>
#include <QPieSeries>
#include <QPieSlice>
#include <QEvent>
#include <QLegend>
#include <algorithm>
#include <cmath>
#include <vector>

// Proportions are a presentation of supplied nonnegative values, never a DSP
// transform. The legend stays bounded even for a large supplied matrix.
class NumericPieView final : public QChartView {
public:
    explicit NumericPieView(QWidget* parent=nullptr):QChartView(new QChart,parent) {
        setObjectName("numericPieView");setRenderHint(QPainter::Antialiasing);
        chart()->addSeries(series_);chart()->legend()->hide();chart()->setAnimationOptions(QChart::NoAnimation);
        chart()->setDropShadowEnabled(false);chart()->setBackgroundRoundness(0);
        connect(&ui::Language::instance(),&ui::Language::changed,this,[this]{rebuild();});refresh_theme();
    }
    bool set_values(const std::vector<float>& values) {
        if(values.size()>1048576){values_.clear();series_->clear();valid_=false;error_="Renderer projection unavailable";refresh_title();return false;}
        values_=values;rebuild();return valid_;
    }
    bool projection_available() const {return valid_;}
    void set_projection_status(const QString& status) {status_=status;refresh_title();}
    void set_render_visible(bool visible) {setUpdatesEnabled(visible);if(visible)update();}
    size_t value_count() const {return values_.size();}
    float peak_value() const {float peak=0;for(float value:values_)if(std::isfinite(value))peak=std::max(peak,std::abs(value));return peak;}
    size_t slice_count() const {return size_t(series_->count());}
    double slice_value(size_t index) const {return index<slice_count()?series_->slices()[int(index)]->value():0;}
    double slice_fraction(size_t index) const {return index<slice_count()?series_->slices()[int(index)]->percentage():0;}
protected:
    void changeEvent(QEvent* event) override {QChartView::changeEvent(event);if(event->type()==QEvent::PaletteChange)refresh_theme();}
private:
    void refresh_title() {
        const auto detail=error_.isEmpty()?QString{}:ui::text(error_.toUtf8().constData());
        chart()->setTitle((!valid_?detail:status_.isEmpty()?detail:status_).toHtmlEscaped());
    }
    void rebuild() {
        series_->clear();error_.clear();valid_=true;
        if(values_.size()>1048576){valid_=false;error_="Renderer projection unavailable";refresh_title();return;}
        if(values_.empty()){refresh_title();return;}
        double sum=0;for(float value:values_) {
            if(!std::isfinite(value) || value<0){valid_=false;error_="Pie charts require finite nonnegative values";refresh_title();return;}
            sum+=double(value);
        }
        if(sum==0){error_="All values are zero";refresh_title();return;}
        const auto individual=std::min(values_.size(),size_t{63});
        for(size_t i=0;i<individual;++i)series_->append(ui::text("Value %1").arg(qulonglong(i)),double(values_[i]));
        if(values_.size()>individual) {
            double other=0;for(size_t i=individual;i<values_.size();++i)other+=double(values_[i]);
            series_->append(ui::text("Other"),other);
        }
        for(auto* slice:series_->slices())slice->setLabelVisible(series_->count()<=12);
        refresh_theme();refresh_title();
    }
    void refresh_theme() {
        chart()->setBackgroundBrush(palette().brush(QPalette::Base));
        chart()->setPlotAreaBackgroundBrush(palette().brush(QPalette::Base));
        chart()->setTitleBrush(palette().brush(QPalette::Text));
        const auto base=palette().color(QPalette::Base),accent=palette().color(QPalette::Highlight);
        const auto slices=series_->slices();const int count=int(slices.size());
        for(int i=0;i<slices.size();++i) {
            const double fraction=.35+.65*double(i)/std::max(1,count-1);
            slices[i]->setBrush(QColor::fromRgbF(base.redF()+(accent.redF()-base.redF())*fraction,
                base.greenF()+(accent.greenF()-base.greenF())*fraction,base.blueF()+(accent.blueF()-base.blueF())*fraction));
            slices[i]->setPen(QPen(palette().color(QPalette::Mid)));slices[i]->setLabelBrush(palette().brush(QPalette::Text));
        }
    }
    QPieSeries* series_=new QPieSeries;
    std::vector<float> values_;
    QString status_,error_;
    bool valid_=true;
};
