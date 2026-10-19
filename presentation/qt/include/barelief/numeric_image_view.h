#pragma once
#include <barelief/spectrogram_axis_metadata.h>
#include <barelief/numeric_plot_limits.h>
#include <barelief/numeric_matrix_snapshot.h>
#include <barelief/amplitude_range.h>
#include <barelief/axis_tick_labels.h>
#include <barelief/ui_text.h>
#include <QImage>
#include <barelief/quick2d/plot_model.h>
#include <barelief/quick2d/plot_ticks.h>
#include <QPaintEvent>
#include <QEvent>

// A bounded GUI projection of [row][column], independent of signal semantics.
// Matrix order is preserved; age_order affects labels only for ring histories.
class NumericImageView final : public QuickPlotWidget {
    Q_OBJECT
public:
    explicit NumericImageView(QWidget* parent=nullptr):QuickPlotWidget(parent) {
        setObjectName("numericImageView");setMinimumSize(160,120);
        connect(&ui::Language::instance(),&ui::Language::changed,this,[this]{refresh_presentation();});
    }
    void set_matrix(const std::vector<std::vector<float>>& rows,bool age_order=false) {
        set_snapshot(NumericMatrixSnapshot::take(rows),age_order);
    }
    void set_snapshot(NumericMatrixSnapshot snapshot,bool age_order=false) {
        if(!snapshot.empty() && !presentation::NumericPlotLimits::accepts(snapshot.size(),snapshot.front().size()))snapshot.clear();
        data_=std::move(snapshot);age_order_=age_order;
        observe_amplitude();
        rebuild();refresh_presentation();
    }
    const void* projection_storage_identity() const noexcept {return data_.storage_identity();}
    bool set_axis_metadata(const SpectrogramAxisMetadata& metadata) {
        if(!valid_axis_metadata(metadata))return false;
        if(source_axes_==metadata)return true;
        const bool scaleChanged=source_axes_.value_scale!=metadata.value_scale;
        source_axes_=metadata;axes_=metadata.displayed(frequency_mode_,time_mode_);
        if(scaleChanged){amplitude_.reset();set_snapshot(data_,age_order_);}else refresh_presentation();
        emit axis_metadata_changed();return true;
    }
    void set_axis_display_modes(const QString& frequencyMode,const QString& timeMode) {
        frequency_mode_=frequencyMode=="positions"?"positions":"coordinates";
        time_mode_=timeMode=="positions"?"positions":"coordinates";
        axes_=source_axes_.displayed(frequency_mode_,time_mode_);refresh_presentation();
    }
    void set_projection_status(const QString& status) {if(status_==status)return;status_=status;refresh_presentation();}
    void set_y_range(bool automatic,double minimum,double maximum,double seconds) {
        if(!std::isfinite(minimum) || !std::isfinite(maximum) || minimum>=maximum)return;
        if(automatic && !automatic_)amplitude_.reset();
        automatic_=automatic;minimum_=minimum;maximum_=maximum;interval_=AmplitudeRange::interval(seconds);
        rebuild();refresh_presentation();
    }
    void set_reverse_axes(bool columns,bool rows) {reverse_columns_=columns;reverse_rows_=rows;refresh_presentation();}
    void set_axis_precision(int precision) {precision_=std::clamp(precision,0,8);refresh_presentation();}
    int axis_precision() const {return precision_;}
    void set_render_visible(bool visible) {QuickPlotWidget::set_render_visible(visible);}
    QSize matrix_dimensions() const {return {int(data_.empty()?0:data_.front().size()),int(data_.size())};}
    size_t value_count() const {return data_.empty()?0:data_.size()*data_.front().size();}
    float peak_value() const {float peak=0;for(size_t row=0;row<data_.size();++row)for(size_t column=0;column<data_[row].size();++column)peak=std::max(peak,std::abs(data_[row][column]));return peak;}
    bool matrix_sample(size_t row,size_t column,float& value) const {
        if(row>=data_.size() || column>=data_[row].size())return false;value=data_[row][column];return true;
    }
    double row_coordinate(size_t row) const {
        if(row>=data_.size())return std::numeric_limits<double>::quiet_NaN();
        return axes_.age(age_order_?data_.size()-1-row:row);
    }
    const SpectrogramAxisMetadata& axis_metadata() const {return axes_;}
    const SpectrogramAxisMetadata& source_axis_metadata() const {return source_axes_;}
    std::pair<double,double> y_range() const {return automatic_?amplitude_.range():std::pair{minimum_,maximum_};}
signals:
    void axis_metadata_changed();
protected:
    void changeEvent(QEvent* event) override {
        QuickPlotWidget::changeEvent(event);if(event->type()==QEvent::PaletteChange){rebuild();refresh_presentation();}
    }
    void refresh_presentation() override {
        if(!rendering_enabled_)return;
        const auto yt=row_ticks();
        const double left=quickPlotLeftMargin(yt,font(),width());
        const auto xt=column_ticks(left);
        setProperty("columnAxisSections",xt.size()-1);setProperty("timeAxisSections",yt.size()-1);
        publish({{"viewLeft",viewport_.left()},{"viewTop",viewport_.top()},{"viewWidth",viewport_.width()},{"viewHeight",viewport_.height()},{"kind","image"},{"available",!pixels_.isNull()},{"title",status_},{"xTicks",xt},{"yTicks",yt},{"xGrid",quickPlotGrid(xt,grid_x_)},{"yGrid",quickPlotGrid(yt,grid_y_)},
            {"xTitle",axis_title(axes_.column_title_key)},{"yTitle",axis_title(axes_.time_title_key)},
            {"plotLeft",left},{"reverseX",reverse_columns_},{"reverseY",reverse_rows_}});
    }
private:
    static bool valid_axis_metadata(const SpectrogramAxisMetadata& metadata) {
        if(metadata.ages.size()>presentation::NumericPlotLimits::rows || metadata.row_indices.size()>presentation::NumericPlotLimits::rows || metadata.columns.size()>presentation::NumericPlotLimits::columns)return false;
        const auto finite=[](double value){return std::isfinite(value);};
        return std::all_of(metadata.ages.begin(),metadata.ages.end(),finite) && std::all_of(metadata.columns.begin(),metadata.columns.end(),finite) && std::all_of(metadata.row_indices.begin(),metadata.row_indices.end(),finite);
    }
    void observe_amplitude() {
        if(!automatic_ || data_.empty())return;
        double low=0,high=0;
        for(size_t row=0;row<data_.size();++row)for(size_t column=0;column<data_[row].size();++column) {
            const double value=axes_.value_scale.map(data_[row][column]);
            low=std::min(low,value);high=std::max(high,value);
        }
        amplitude_.observe(low,high,AmplitudeRange::now(),interval_);
    }
    static QString axis_title(const QString& key) {
        auto text=ui::text(key.toUtf8().constData());
        for(const auto* suffix:{" (X)"," (Y)"," (Z)"})if(text.endsWith(suffix)){text.chop(4);break;}
        return text;
    }
    QVariantList row_ticks() const {
        const auto last=data_.empty()?0:data_.size()-1;
        const bool integral=time_mode_=="positions" || (!axes_.time_coordinates && (axes_.age_axis || axes_.ages.empty()));
        const auto placement=[&](double f){return viewport_.height()==1 && integral && last?std::round(f*double(last))/double(last):f;};
        const auto coordinate=[&](double f) {
            const double index=(viewport_.top()+(reverse_rows_?1-f:f)*viewport_.height())*double(last);
            return axes_.age(age_order_?double(last)-index:index);
        };
        return quickPlotTicks(height()-86,font(),precision_,coordinate,false,int(std::min(size_t{32},last)),placement,labels_y_);
    }
    QVariantList column_ticks(double left) const {
        const auto columns=data_.empty()?0:data_.front().size();
        const auto last=columns?columns-1:0;
        const auto placement=[&](double f){return viewport_.width()==1 && !axes_.column_coordinates && last?std::round(f*double(last))/double(last):f;};
        const auto coordinate=[&](double f){return axes_.column((viewport_.left()+(reverse_columns_?1-f:f)*viewport_.width())*double(last));};
        return quickPlotTicks(width()-left-22,font(),precision_,coordinate,true,int(std::min(size_t{32},last)),placement,labels_x_);
    }
    static QRgb pixel(double fraction,const QColor& base,const QColor& accent) {
        return qRgb(qRound(base.red()+(accent.red()-base.red())*fraction),qRound(base.green()+(accent.green()-base.green())*fraction),qRound(base.blue()+(accent.blue()-base.blue())*fraction));
    }
    void fill_pixel_row(int row,const std::pair<double,double>& bounds,const QColor& base,const QColor& accent) {
        auto* scan=reinterpret_cast<QRgb*>(pixels_.scanLine(row));
        for(int column=0;column<pixels_.width();++column) {
            const double fraction=std::clamp((axes_.value_scale.map(data_[row][column])-bounds.first)/std::max(1e-300,bounds.second-bounds.first),0.,1.);
            scan[column]=pixel(fraction,base,accent);
        }
    }
    void rebuild() {
        if(data_.empty() || data_.front().empty()){pixels_={};publish_image(pixels_);return;}
        pixels_=QImage(int(data_.front().size()),int(data_.size()),QImage::Format_RGB32);
        // Match the shared QML data palette explicitly. A non-activating dock
        // can inherit QWidget's Inactive group, whose accent may be grey.
        const auto colors=ui::Theme::instance().palette();
        const auto bounds=y_range();
        const auto base=colors.color(QPalette::Active,QPalette::Base),accent=colors.color(QPalette::Active,QPalette::Highlight);
        for(int row=0;row<pixels_.height();++row)fill_pixel_row(row,bounds,base,accent);
        publish_image(pixels_);
    }
    NumericMatrixSnapshot data_;
    SpectrogramAxisMetadata axes_;
    SpectrogramAxisMetadata source_axes_;
    QString frequency_mode_="coordinates",time_mode_="coordinates";
    QImage pixels_;
    QString status_;
    AmplitudeRange amplitude_;
    bool automatic_=true,age_order_=false,reverse_columns_=false,reverse_rows_=false;
    double minimum_=0,maximum_=1,interval_=10;
    int precision_=0;
};
