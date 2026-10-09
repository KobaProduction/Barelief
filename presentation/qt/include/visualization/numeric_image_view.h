#pragma once
#include <visualization/spectrograms/spectrogram_graph.h>
#include <visualization/axis_tick_labels.h>
#include <visualization/ui_text.h>
#include <QImage>
#include <QPainter>
#include <QPaintEvent>
#include <QEvent>

// A bounded GUI projection of [row][column], independent of signal semantics.
// Matrix order is preserved; age_order affects labels only for ring histories.
class NumericImageView final : public QWidget {
    Q_OBJECT
public:
    explicit NumericImageView(QWidget* parent=nullptr):QWidget(parent) {
        setObjectName("numericImageView");setMinimumSize(160,120);
        connect(&ui::Language::instance(),&ui::Language::changed,this,[this]{update();});
    }
    void set_matrix(const std::vector<std::vector<float>>& rows,bool age_order=false) {
        set_snapshot(NumericMatrixSnapshot::take(rows),age_order);
    }
    void set_snapshot(NumericMatrixSnapshot snapshot,bool age_order=false) {
        if(!snapshot.empty() && (snapshot.size()>16384 || snapshot.front().size()>4096))snapshot.clear();
        data_=std::move(snapshot);age_order_=age_order;
        if(automatic_ && !data_.empty()) {
            double low=0,high=0;
            for(size_t row=0;row<data_.size();++row)for(size_t column=0;column<data_[row].size();++column) {
                const float value=data_[row][column];low=std::min(low,double(value));high=std::max(high,double(value));
            }
            amplitude_.observe(low,high,AmplitudeRange::now(),interval_);
        }
        rebuild();update();
    }
    const void* projection_storage_identity() const noexcept {return data_.storage_identity();}
    bool set_axis_metadata(const SpectrogramAxisMetadata& metadata) {
        if(metadata.ages.size()>16384 || metadata.row_indices.size()>16384 || metadata.columns.size()>4096)return false;
        for(double value:metadata.ages)if(!std::isfinite(value))return false;
        for(double value:metadata.columns)if(!std::isfinite(value))return false;
        for(double value:metadata.row_indices)if(!std::isfinite(value))return false;
        const bool changed=source_axes_!=metadata;
        source_axes_=metadata;axes_=metadata.displayed(frequency_mode_,time_mode_);update();
        if(changed)emit axis_metadata_changed();return true;
    }
    void set_axis_display_modes(const QString& frequencyMode,const QString& timeMode) {
        frequency_mode_=frequencyMode=="positions"?"positions":"coordinates";
        time_mode_=timeMode=="positions"?"positions":"coordinates";
        axes_=source_axes_.displayed(frequency_mode_,time_mode_);update();
    }
    void set_projection_status(const QString& status) {status_=status;update();}
    void set_y_range(bool automatic,double minimum,double maximum,double seconds) {
        if(!std::isfinite(minimum) || !std::isfinite(maximum) || minimum>=maximum)return;
        if(automatic && !automatic_)amplitude_.reset();
        automatic_=automatic;minimum_=minimum;maximum_=maximum;interval_=AmplitudeRange::interval(seconds);
        rebuild();update();
    }
    void set_reverse_axes(bool columns,bool rows) {reverse_columns_=columns;reverse_rows_=rows;update();}
    void set_axis_precision(int precision) {precision_=std::clamp(precision,0,8);update();}
    int axis_precision() const {return precision_;}
    void set_render_visible(bool visible) {setUpdatesEnabled(visible);if(visible)update();}
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
        QWidget::changeEvent(event);if(event->type()==QEvent::PaletteChange){rebuild();update();}
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);painter.fillRect(rect(),palette().brush(QPalette::Base));
        painter.setPen(palette().color(QPalette::Text));
        const QLocale locale(ui::Language::instance().code()=="ru"?"ru_RU":"en_US");
        int rowLabelWidth=0;
        if(!data_.empty())for(const auto& label:axisTickLabels({row_coordinate(0),row_coordinate(data_.size()-1)},locale,precision_))
            rowLabelWidth=std::max(rowLabelWidth,QFontMetrics(painter.font()).horizontalAdvance(label));
        const double margin=std::min(std::max(76,rowLabelWidth+30),std::max(76,width()/2));
        const QRectF plot=QRectF(rect()).adjusted(margin,16,-20,-54);
        if(plot.width()<=0 || plot.height()<=0)return;
        if(pixels_.isNull())painter.drawText(plot,Qt::AlignCenter,ui::text("Renderer projection unavailable"));
        else {
            painter.save();painter.translate(plot.left()+(reverse_columns_?plot.width():0),plot.top()+(reverse_rows_?plot.height():0));
            painter.scale(reverse_columns_?-1:1,reverse_rows_?-1:1);
            painter.drawImage(QRectF(0,0,plot.width(),plot.height()),pixels_);painter.restore();
            painter.setPen(palette().color(QPalette::Mid));painter.drawRect(plot);
            painter.setPen(palette().color(QPalette::Text));
            const QFontMetrics metrics(painter.font());
            const auto drawTicks=[&](bool horizontal) {
                const int samples=horizontal?pixels_.width():pixels_.height();
                const bool reverse=horizontal?reverse_columns_:reverse_rows_;
                const auto coordinate=[&](double fraction) {
                    const double index=(reverse?1-fraction:fraction)*std::max(0,samples-1);
                    return horizontal?axes_.column(index):axes_.age(age_order_?data_.size()-1-index:index);
                };
                int sections=std::clamp(int((horizontal?plot.width():plot.height())/(horizontal?60.:metrics.height()+10.)),1,std::min(32,std::max(1,samples-1)));
                QStringList labels;
                for(;;) {
                    std::vector<double> values;for(int tick=0;tick<=sections;++tick)values.push_back(coordinate(double(tick)/sections));
                    labels=axisTickLabels(values,locale,precision_);
                    int extent=metrics.height();if(horizontal)for(const auto& label:labels)extent=std::max(extent,metrics.horizontalAdvance(label));
                    if(sections==1 || (horizontal?plot.width():plot.height())/sections>=extent+12)break;
                    --sections;
                }
                setProperty(horizontal?"columnAxisSections":"timeAxisSections",sections);
                for(int tick=0;tick<=sections;++tick) {
                    const double fraction=double(tick)/sections;
                    if(horizontal){
                        const int extent=metrics.horizontalAdvance(labels[tick])+4;
                        const double left=std::clamp(plot.left()+fraction*plot.width()-extent/2.,0.,std::max(0.,double(width()-extent)));
                        painter.drawText(QRectF(left,plot.bottom()+3,extent,metrics.height()),Qt::AlignCenter,labels[tick]);
                    }else painter.drawText(QRectF(24,plot.top()+fraction*plot.height()-metrics.height()/2.,margin-28,metrics.height()),Qt::AlignRight|Qt::AlignVCenter,labels[tick]);
                }
            };
            drawTicks(true);drawTicks(false);
            const auto title=[](const QString& key){auto text=ui::text(key.toUtf8().constData());for(const auto* suffix:{" (X)"," (Y)"," (Z)"})if(text.endsWith(suffix)){text.chop(4);break;}return text;};
            const auto columnTitle=title(axes_.column_title_key);
            painter.drawText(QRectF(plot.left(),plot.bottom()+23,plot.width(),20),Qt::AlignCenter,
                QFontMetrics(painter.font()).elidedText(columnTitle,Qt::ElideRight,int(plot.width())));
            painter.save();painter.translate(15,plot.center().y());painter.rotate(-90);
            painter.drawText(QRectF(-plot.height()/2,-9,plot.height(),18),Qt::AlignCenter,
                QFontMetrics(painter.font()).elidedText(title(axes_.time_title_key),Qt::ElideRight,int(plot.height())));painter.restore();
        }
        if(!status_.isEmpty())painter.drawText(QRectF(8,1,width()-16,14),Qt::AlignLeft,
            QFontMetrics(painter.font()).elidedText(status_,Qt::ElideRight,std::max(1,width()-16)));
    }
private:
    void rebuild() {
        if(data_.empty() || data_.front().empty()){pixels_={};return;}
        pixels_=QImage(int(data_.front().size()),int(data_.size()),QImage::Format_RGB32);
        const auto bounds=y_range();const auto base=palette().color(QPalette::Base),accent=palette().color(QPalette::Highlight);
        for(int row=0;row<pixels_.height();++row) {
            auto* scan=reinterpret_cast<QRgb*>(pixels_.scanLine(row));
            for(int column=0;column<pixels_.width();++column) {
                const double fraction=std::clamp((double(data_[row][column])-bounds.first)/std::max(1e-300,bounds.second-bounds.first),0.,1.);
                scan[column]=qRgb(qRound(base.red()+(accent.red()-base.red())*fraction),
                    qRound(base.green()+(accent.green()-base.green())*fraction),qRound(base.blue()+(accent.blue()-base.blue())*fraction));
            }
        }
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
