#include <barelief/compact_spectrogram_view.h>
#include <barelief/ui_text.h>
#include <barelief/classic_spectrogram_colors.h>
#include <QPainter>
#include <QJsonArray>
#include <QEvent>
#include <QTimer>
#include <array>

ClassicSpectrogramWidget::ClassicSpectrogramWidget(QWidget* parent):QWidget(parent) {
    setObjectName("classicSpectrogramView");setMinimumSize(480,240);
    fps_clock_.start();fps_timer_=new QTimer(this);fps_timer_->setInterval(500);
    connect(fps_timer_,&QTimer::timeout,this,&ClassicSpectrogramWidget::sample_frame_rate);fps_timer_->start();
    connect(&ui::Language::instance(),&ui::Language::changed,this,qOverload<>(&QWidget::update));
}
bool CompactSpectrogramView::set_sections(const QJsonArray& sections) {
    if(sections.isEmpty() || sections.size()>16)return false;
    std::vector<Section> next;
    for(const auto& value:sections){
        const int begin=next.empty()?0:next.back().end;
        const auto section=value.toObject();const int count=section.value("count").toInt(-1);
        const auto label=section.value("label").toString().toUtf8();
        if(count<1 || count>48-begin || section.value("begin").toInt(-1)!=begin || label.isEmpty())return false;
        next.push_back({begin,begin+count,label});
    }
    if(next.back().end!=48)return false;sections_=std::move(next);return true;
}
QString CompactSpectrogramView::section_label(size_t index) const {
    return index<sections_.size()?ui::text(sections_[index].label.constData()):QString{};
}
std::pair<int,int> CompactSpectrogramView::section_range(size_t index) const {
    return index<sections_.size()?std::pair{sections_[index].begin,sections_[index].end}:std::pair{0,0};
}
std::pair<int,int> CompactSpectrogramView::semantic_section(size_t band,bool notes) const {
    if(notes)return {48,96};
    for(const auto& section:sections_)if(band<size_t(section.end))return {section.begin,section.end};
    return {0,48};
}
namespace {
std::vector<double> decode_band_centers(const QJsonArray& values) {
    std::vector<double> centers;
    for(const auto& item:values) {
        if(!item.isDouble() || !std::isfinite(item.toDouble()) || item.toDouble()<0)return {};
        if(!centers.empty() && item.toDouble()<=centers.back())return {};
        centers.push_back(item.toDouble());
    }
    return centers;
}
}
bool CompactSpectrogramView::set_layout(const QJsonObject& layout) {
    const auto values=layout.value("broadband_centers_hz").toArray();
    const int midi=layout.value("first_midi").toInt(-1);
    if(values.size()!=48 || midi<0 || midi>80)return false;
    auto centers=decode_band_centers(values);
    if(centers.size()!=48 || !set_sections(layout.value("broadband_sections").toArray()))return false;
    boundaries_=std::move(centers);first_midi_=unsigned(midi);update();return true;
}
void ClassicSpectrogramWidget::set_snapshot(NumericMatrixSnapshot data,bool age_order) {
    if(!data.empty() && (data.size()>512 || !accepts_columns(data.front().size())))data.clear();
    data_=std::move(data);age_order_=age_order;rebuild_raster();if(render_visible_)update();
}
bool ClassicSpectrogramWidget::set_axis_metadata(const SpectrogramAxisMetadata& axes) {
    if(axes.ages.size()>512 || axes.columns.size()>4096)return false;
    for(double value:axes.ages)if(!std::isfinite(value))return false;
    axes_=axes;if(render_visible_)update();emit axis_metadata_changed();return true;
}
void ClassicSpectrogramWidget::set_projection_status(const QString& value) {
    if(status_==value)return;status_=value;if(render_visible_)update();
}
void ClassicSpectrogramWidget::set_value_range(double minimum,double maximum) {
    if(!std::isfinite(minimum) || !std::isfinite(maximum) || minimum>=maximum)return;
    if(minimum_==minimum && maximum_==maximum)return;
    minimum_=minimum;maximum_=maximum;rebuild_raster();if(render_visible_)update();
}
void ClassicSpectrogramWidget::set_orientation(const QString& value) {
    const bool horizontal=value=="horizontal";if(horizontal_==horizontal)return;horizontal_=horizontal;rebuild_raster();update();
}
void ClassicSpectrogramWidget::set_render_visible(bool visible) {
    if(render_visible_==visible)return;
    render_visible_=visible;measured_fps_=0;sampled_frames_=painted_frames_;fps_clock_.restart();
    if(visible){fps_timer_->start();update();}else fps_timer_->stop();
}
void ClassicSpectrogramWidget::sample_frame_rate() {
    const auto elapsed=fps_clock_.nsecsElapsed();fps_clock_.restart();
    measured_fps_=elapsed>0?qreal(painted_frames_-sampled_frames_)*1e9/qreal(elapsed):0;
    sampled_frames_=painted_frames_;
}
qreal ClassicSpectrogramWidget::display_fps() const {return render_visible_ && isVisible()?measured_fps_:0;}
QPen ClassicSpectrogramWidget::axis_pen() const {return QPen(palette().color(QPalette::Text),1);}

double ClassicSpectrogramWidget::frequency(size_t band) const {return axes_.column(double(band));}
double CompactSpectrogramView::frequency(size_t band) const {
    return boundaries_.size()==48 && band<48?boundaries_[band]:std::numeric_limits<double>::quiet_NaN();
}
QString CompactSpectrogramView::note_label(size_t band) const {
    if(band>=48)return {};
    static constexpr std::array names{"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    const unsigned midi=first_midi_+unsigned(band);
    return QString::fromLatin1(names[midi%12])+QString::number(int(midi/12)-1);
}
double ClassicSpectrogramWidget::age(size_t row) const {
    if(row>=data_.size())return std::numeric_limits<double>::quiet_NaN();
    return axes_.age(age_order_?double(data_.size()-1-row):double(row));
}
bool ClassicSpectrogramWidget::sample(size_t row,size_t column,float& value) const {
    if(row>=data_.size() || column>=(data_.empty()?0:data_.front().size()))return false;value=data_[row][column];return true;
}
float ClassicSpectrogramWidget::peak_value() const {
    float peak=0;
    for(size_t row=0;row<data_.size();++row)for(size_t column=0;column<data_.front().size();++column)peak=std::max(peak,data_[row][column]);
    return peak;
}
namespace {
QString plain_number(double value) {
    if(!std::isfinite(value))return {};
    return QString::number(value,'f',std::abs(value)<10?2:std::abs(value)<100?1:0);
}
}
QRectF ClassicSpectrogramWidget::plot_rect(int) const {
    return {78,32.,double(std::max(1.,width()-148-legend_offset())),double(std::max(1,height()-(horizontal_?100:vertical_margin())))};
}
void ClassicSpectrogramWidget::fill_raster_band(int band) {
    static const auto colors=classicSpectrogramColors();
    const auto frames=size_t(horizontal_?raster_.width():raster_.height());
    const auto bands=size_t(horizontal_?raster_.height():raster_.width());
    for(size_t frame=0;frame<frames;++frame) {
        const double fraction=std::clamp((raster_peak(frame,size_t(band),frames,bands)-minimum_)/(maximum_-minimum_),0.,1.);
        const int x=horizontal_?int(frame):band,y=horizontal_?int(bands)-1-band:int(frame);
        raster_.setPixel(x,y,colors[size_t(std::lround(fraction*255))]);
    }
}
float ClassicSpectrogramWidget::raster_peak(size_t frame,size_t band,size_t frames,size_t bands) const {
    const auto columns=data_.front().size();
    float peak=std::numeric_limits<float>::lowest();
    for(size_t row=frame*data_.size()/frames;row<(frame+1)*data_.size()/frames;++row)
        for(size_t column=band*columns/bands;column<(band+1)*columns/bands;++column)
            peak=std::max(peak,data_[row][column]);
    return peak;
}
void ClassicSpectrogramWidget::rebuild_raster() {
    if(data_.empty()){raster_={};return;}
    const auto rect=plot_rect(0);const auto scale=devicePixelRatioF();
    const int bands=std::min(int(data_.front().size()),std::max(1,int(std::floor((horizontal_?rect.height():rect.width())*scale))));
    const int frames=std::min(int(data_.size()),std::max(1,int(std::floor((horizontal_?rect.width():rect.height())*scale))));
    raster_=QImage(horizontal_?frames:bands,horizontal_?bands:frames,QImage::Format_RGB32);
    for(int band=0;band<bands;++band)fill_raster_band(band);
}
void CompactSpectrogramView::draw_range_ticks(QPainter& painter,const QRectF& rect,int begin,int count,bool notes) {
    const double pixels=rect.width()*count/96;
    if(!notes && pixels<120){draw_band_tick(painter,rect,false,size_t(begin+count/2));return;}
    const int sections=std::clamp(int(pixels/(notes?42:65)),1,count-1);
    for(int tick=0;tick<=sections;++tick)draw_band_tick(painter,rect,notes,size_t(begin+std::lround((count-1.)*tick/sections)));
}
void CompactSpectrogramView::draw_group_ticks(QPainter& painter,const QRectF& rect,bool notes) {
    if(notes){draw_range_ticks(painter,rect,0,48,true);return;}
    for(const auto& section:sections_)draw_range_ticks(painter,rect,section.begin,section.end-section.begin,false);
}
void CompactSpectrogramView::draw_band_tick(QPainter& painter,const QRectF& rect,bool notes,size_t band) {
        const double x=rect.left()+(double(band)+(notes?48:0)+.5)*rect.width()/96;
        const double hz=frequency(band);
        const auto label=notes?note_label(band):hz>=1000?QString::number(hz/1000,'f',hz<10000?1:0)+"k":plain_number(hz);
        const double span=std::min(rect.width()/4,double(painter.fontMetrics().horizontalAdvance(label)+8));
        const auto [begin,end]=semantic_section(band,notes);
        const double first=rect.left()+begin*rect.width()/96,last=rect.left()+end*rect.width()/96;
        if(span>last-first-4)return;
        const double left=std::clamp(x-span/2,first+2,last-span-2);
        if(horizontal_)painter.drawText(QRectF(rect.right()+8,rect.bottom()-(double(band)+(notes?48:0)+.5)*rect.height()/96-10,78,20),Qt::AlignLeft,label);
        else painter.drawText(QRectF(left,rect.bottom()+76,span,20),Qt::AlignHCenter,label);
        painter.drawLine(QPointF(x,rect.bottom()+66),QPointF(x,rect.bottom()+72));
}

void CompactSpectrogramView::draw_index_axis(QPainter& painter,const QRectF& rect) {
    for(const int index:index_ticks()) {
        const double x=rect.left()+(index+.5)*rect.width()/96;
        if(horizontal_)painter.drawText(QRectF(22,rect.bottom()-(index+.5)*rect.height()/96-10,48,20),Qt::AlignRight,QString::number(index));
        else painter.drawText(QRectF(x-18,rect.bottom()+22,36,20),Qt::AlignHCenter,QString::number(index));
    }

}
std::vector<int> CompactSpectrogramView::index_ticks() const {
    std::vector<int> result;for(const auto& section:sections_)result.push_back(section.begin);
    for(int index:{48,60,72,84,95})result.push_back(index);
    return result;
}
void ClassicSpectrogramWidget::draw_group(QPainter& painter,int) {
    const auto rect=plot_rect(0);
    if(!raster_.isNull())painter.drawImage(rect,raster_);
    painter.drawRect(rect);
    draw_axes(painter,rect);
}
void ClassicSpectrogramWidget::draw_axes(QPainter& painter,const QRectF& rect) {
    if(horizontal_){draw_classical_axes(painter,rect);return;}
    const auto bands=data_.empty()?0:data_.front().size();
    for(int tick=0;tick<=5;++tick) {
        const auto band=size_t(std::lround(double(bands?bands-1:0)*tick/5));
        const double x=rect.left()+(double(band)+.5)*rect.width()/std::max(size_t{1},bands);
        painter.drawText(QRectF(x-32,rect.bottom()+7,64,20),Qt::AlignHCenter,plain_number(frequency(band)));
    }
    painter.drawText(QRectF(rect.left(),rect.bottom()+31,rect.width(),22),Qt::AlignHCenter,ui::text(axes_.column_coordinates?"Frequency, Hz":"Channel index"));
    draw_time_axis(painter,rect);
}
void CompactSpectrogramView::draw_index_grid(QPainter& painter,const QRectF& rect) {
    painter.save();auto color=palette().color(QPalette::Mid);color.setAlpha(80);
    painter.setPen(QPen(color,1,Qt::DotLine));
    for(int index:{60,72,84}){
        const double x=rect.left()+index*rect.width()/96;
        painter.drawLine(QPointF(x,rect.top()),QPointF(x,rect.bottom()));
    }
    painter.restore();
}
void CompactSpectrogramView::draw_axis_frames(QPainter& painter,const QRectF& rect) {
    painter.save();painter.setPen(axis_pen());
    painter.drawLine(QPointF(rect.left(),rect.bottom()+12),QPointF(rect.right(),rect.bottom()+12));
    for(const int index:index_ticks()) {
        const double x=rect.left()+(index+.5)*rect.width()/96;
        painter.drawLine(QPointF(x,rect.bottom()+12),QPointF(x,rect.bottom()+18));
    }
    painter.setPen(axis_pen());
    for(size_t i=0;i<=sections_.size();++i) {
        const auto range=i<sections_.size()?section_range(i):std::pair{48,96};
        const double left=rect.left()+range.first*rect.width()/96,right=rect.left()+range.second*rect.width()/96;
        painter.drawLine(QPointF(left,rect.bottom()+66),QPointF(right,rect.bottom()+66));
        painter.drawLine(QPointF(left,rect.bottom()+61),QPointF(left,rect.bottom()+71));

    }
    painter.drawLine(QPointF(rect.right(),rect.bottom()+61),QPointF(rect.right(),rect.bottom()+71));
    painter.restore();
}
void CompactSpectrogramView::draw_sections(QPainter& painter,const QRectF& rect) {
    const bool angled=std::any_of(sections_.begin(),sections_.end(),[&](const auto& section){
        return (section.end-section.begin)*rect.width()/96<painter.fontMetrics().horizontalAdvance(ui::text(section.label.constData()))+8;
    });
    for(const auto& section:sections_) {
        const double x=rect.left()+section.begin*rect.width()/96;
        if(section.begin)painter.drawLine(QPointF(x,rect.top()),QPointF(x,rect.bottom()));
        draw_section_label(painter,rect,section,angled);
    }
    painter.drawText(QRectF(rect.center().x(),rect.bottom()+106,rect.width()/2,22),Qt::AlignHCenter,ui::text("Notes"));
}
void CompactSpectrogramView::draw_section_label(QPainter& painter,const QRectF& rect,const Section& section,bool angled) {
    const auto label=ui::text(section.label.constData());
    const double first=rect.left()+section.begin*rect.width()/96,span=(section.end-section.begin)*rect.width()/96;
    if(!angled){painter.drawText(QRectF(first,rect.bottom()+106,span,22),Qt::AlignHCenter,label);return;}
    painter.save();painter.translate(first+span/2,rect.bottom()+110);painter.rotate(45);
    painter.drawText(QRectF(0,-10,painter.fontMetrics().horizontalAdvance(label)+4,22),Qt::AlignLeft,label);painter.restore();
}
void CompactSpectrogramView::draw_axes(QPainter& painter,const QRectF& rect) {
    if(horizontal_)painter.drawLine(QPointF(rect.left(),rect.center().y()),QPointF(rect.right(),rect.center().y()));
    else painter.drawLine(QPointF(rect.center().x(),rect.top()),QPointF(rect.center().x(),rect.bottom()));
    if(!horizontal_){draw_index_grid(painter,rect);draw_axis_frames(painter,rect);draw_sections(painter,rect);}
    draw_group_ticks(painter,rect,false);draw_group_ticks(painter,rect,true);draw_index_axis(painter,rect);
    if(horizontal_)draw_horizontal_time_axis(painter,rect);else draw_time_axis(painter,rect);
}

void ClassicSpectrogramWidget::draw_time_axis(QPainter& painter,const QRectF& rect) {
    const auto rows=data_.size();
    for(int tick=0;tick<=4;++tick) {
        const auto row=rows?size_t(std::lround(double(rows-1)*tick/4)):0;
        const double y=rect.top()+(double(row)+.5)*rect.height()/std::max(size_t{1},rows);
        painter.drawText(QRectF(22,y-10,48,20),Qt::AlignRight|Qt::AlignVCenter,rows?plain_number(age(row)):QString{});
    }
    painter.save();painter.translate(15,rect.center().y());painter.rotate(-90);
    painter.drawText(QRectF(-rect.height()/2,-12,rect.height(),22),Qt::AlignHCenter,ui::text(axes_.time_coordinates?"Age, seconds":"Frame age"));painter.restore();
}
void ClassicSpectrogramWidget::draw_horizontal_time_axis(QPainter& painter,const QRectF& rect) {
    const auto rows=data_.size();
    for(int tick=0;tick<=4;++tick) {
        const double f=tick/4.;
        painter.drawText(QRectF(rect.left()+f*rect.width()-25,rect.bottom()+7,50,20),Qt::AlignHCenter,rows?plain_number(age(size_t(std::lround(f*double(rows-1))))):QString{});
    }
    painter.drawText(QRectF(rect.left(),rect.bottom()+31,rect.width(),22),Qt::AlignHCenter,ui::text(axes_.time_coordinates?"Age, seconds":"Frame age"));
}
void ClassicSpectrogramWidget::draw_classical_axes(QPainter& painter,const QRectF& rect) {
    const auto bands=data_.empty()?0:data_.front().size();
    for(int tick=0;tick<=4;++tick) {
        const double f=tick/4.;
        painter.drawText(QRectF(18,rect.bottom()-f*rect.height()-10,52,20),Qt::AlignRight|Qt::AlignVCenter,plain_number(frequency(size_t(std::lround(f*double(bands?bands-1:0))))));
    }
    draw_horizontal_time_axis(painter,rect);
    painter.save();painter.translate(15,rect.center().y());painter.rotate(-90);
    painter.drawText(QRectF(-rect.height()/2,-12,rect.height(),22),Qt::AlignHCenter,ui::text(axes_.column_coordinates?"Frequency, Hz":"Channel index"));painter.restore();
}
void ClassicSpectrogramWidget::draw_value_scale(QPainter& painter) {
    const auto rect=plot_rect(0);const QRectF legend(rect.right()+legend_offset(),rect.top(),14,rect.height());
    QLinearGradient gradient(legend.bottomLeft(),legend.topLeft());gradient.setStops(classicSpectrogramStops());
    painter.fillRect(legend,gradient);painter.drawRect(legend);
    for(int tick=0;tick<=5;++tick) {
        const double fraction=tick/5.;const double y=legend.bottom()-fraction*legend.height();
        painter.drawText(QRectF(legend.right()+6,y-10,45,20),Qt::AlignLeft|Qt::AlignVCenter,plain_number(minimum_+fraction*(maximum_-minimum_)));
    }
}
void ClassicSpectrogramWidget::paintEvent(QPaintEvent*) {
    ++painted_frames_;
    QPainter painter(this);painter.fillRect(rect(),palette().brush(QPalette::Base));painter.setPen(axis_pen());
    if(!ready()){painter.drawText(rect(),Qt::AlignCenter,ui::text("Configure broadband frequency boundaries"));return;}
    draw_group(painter,0);draw_value_scale(painter);
    if(!status_.isEmpty())painter.drawText(QRectF(74,height()-20,width()-138,20),Qt::AlignLeft,status_);
}
void ClassicSpectrogramWidget::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if(event->type()==QEvent::DevicePixelRatioChange)rebuild_raster();
    if(event->type()==QEvent::PaletteChange || event->type()==QEvent::DevicePixelRatioChange)update();
}
void ClassicSpectrogramWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);rebuild_raster();
}
