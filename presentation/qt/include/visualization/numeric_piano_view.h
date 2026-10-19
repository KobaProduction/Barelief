#pragma once
#include <visualization/quick2d/plot_model.h>
#include <visualization/render_preparation_policy.h>
#include <visualization/rendering/qt_frame_pump.h>
#include <visualization/ui_text.h>
#include <QShowEvent>
#include <QHideEvent>
#include <algorithm>
#include <cmath>
#include <vector>

// Supplied note values remain in C++; the retained Quick scene receives only
// bounded key geometry, visual levels and exact caption strings.
class NumericPianoView final : public QuickPlotWidget {
public:
    explicit NumericPianoView(QWidget* parent=nullptr):QuickPlotWidget(presentation::RenderPreparationPolicy::piano_host,parent),pump_([this](auto token){
        mark_submission(token);publish_presentation();
    }) {
        setObjectName("numericPianoView");setMinimumSize(160,120);
        pump_.setVisible(false);publish_presentation();
    }
    ~NumericPianoView() override {pump_.setVisible(false);}
    bool set_values(const std::vector<float>& values){
        if(values.size()>128-first_ || std::any_of(values.begin(),values.end(),[](float v){return !std::isfinite(v)||v<0;})){
            const auto error=ui::text("Note activity requires finite nonnegative values within MIDI 0–127");
            if(!values_.empty() || status_!=error){values_.clear();status_=error;layout_dirty_=true;refresh_presentation();}
            return false;
        }
        if(values_==values)return true;
        layout_dirty_|=values_.size()!=values.size();values_=values;refresh_presentation();return true;
    }
    bool set_frequencies(const std::vector<double>& hz){
        if(std::any_of(hz.begin(),hz.end(),[](double v){return !std::isfinite(v)||v<=0;}))return false;
        if(frequencies_!=hz){frequencies_=hz;refresh_presentation();}return true;
    }
    void set_first_midi(unsigned first){first=std::min(first,127u);if(first_==first)return;first_=first;values_.clear();frequencies_.clear();layout_dirty_=true;refresh_presentation();}
    unsigned first_midi() const{return first_;}
    void set_activity_floor(double value){if(!std::isfinite(value) || value<0 || value>1e30 || activity_floor_==value)return;activity_floor_=value;refresh_presentation();}
    double activity_floor() const{return activity_floor_;}
    size_t value_count() const{return values_.size();}
    float peak_value() const{float value=0;for(auto v:values_)value=std::max(value,v);return value;}
    float note_value(unsigned midi) const{return midi>=first_ && midi-first_<values_.size()?values_[midi-first_]:0;}
    void set_projection_status(const QString& status){if(status_==status)return;status_=status;refresh_presentation();}
    void set_render_fps(int fps){pump_.setRate(fps);}
    void set_render_visible(bool visible){QuickPlotWidget::set_render_visible(visible);pump_.setVisible(visible && isVisible());}
    qreal display_fps() const{return rendered_fps();}
    quint64 presentation_revision() const{return revision_;}
    static bool black_key(unsigned midi){const auto key=midi%12;return key==1||key==3||key==6||key==8||key==10;}
    QString note_name(unsigned midi) const{static const char* names[]{"C","C♯","D","D♯","E","F","F♯","G","G♯","A","A♯","B"};return QString::fromUtf8(names[midi%12])+QString::number(int(midi/12)-1);}
    QRectF key_rect(unsigned midi) const{
        if(values_.empty() || midi<first_ || midi>=first_+values_.size())return {};
        const auto bounds=keyboard_bounds();
        const auto [whites,index]=white_key_positions(midi);
        const auto width=bounds.width()/std::max(1u,whites);
        return black_key(midi)?QRectF(bounds.left()+index*width-width*.31,bounds.top(),width*.62,bounds.height()*.62):QRectF(bounds.left()+index*width,bounds.top(),width,bounds.height());
    }
protected:
    void frame_completed(quint64 token) override {pump_.complete(token);}
    void refresh_presentation() override {++revision_;pump_.request();}
    void resizeEvent(QResizeEvent* event) override {layout_dirty_=true;QuickPlotWidget::resizeEvent(event);}
    void changeEvent(QEvent* event) override {QuickPlotWidget::changeEvent(event);if(event->type()==QEvent::FontChange)refresh_presentation();}
    void showEvent(QShowEvent* event) override {QWidget::showEvent(event);pump_.setVisible(rendering_enabled_);if(rendering_enabled_)refresh_presentation();}
    void hideEvent(QHideEvent* event) override {pump_.setVisible(false);QWidget::hideEvent(event);}
private:
    std::pair<unsigned,unsigned> white_keyboard_bounds() const {
        auto begin=first_;if(black_key(begin))--begin;
        auto end=unsigned(first_+values_.size()-1);if(black_key(end))++end;
        return {begin,end};
    }
    std::pair<unsigned,unsigned> white_key_positions(unsigned midi) const {
        const auto [begin,end]=white_keyboard_bounds();
        unsigned whites=0,index=0;
        for(auto key=begin;key<=end;++key) {
            if(black_key(key))continue;
            if(key<midi)++index;
            ++whites;
        }
        return {whites,index};
    }
    void rebuild_keys(){
        if(!layout_dirty_)return;
        keys_.clear();keys_.reserve(qsizetype(values_.size()));
        for(unsigned i=0;i<values_.size();++i){
            const auto midi=first_+i;const auto r=key_rect(midi);
            keys_.append(QVariantMap{{"x",r.x()},{"y",r.y()},{"width",r.width()},{"height",r.height()},
                {"black",black_key(midi)},{"label",!black_key(midi)&&(midi%12==0 || values_.size()<=24)?note_name(midi):QString{}}});
        }
        layout_dirty_=false;
    }
    QVariantList activity_levels() const {
        const auto peak=peak_value();
        QVariantList levels;levels.reserve(qsizetype(values_.size()));
        for(auto value:values_)levels.append(value>activity_floor_?double(value)/peak:0.);
        return levels;
    }
    QString peak_caption() const {
        if(values_.empty())return {};
        const auto at=unsigned(std::max_element(values_.begin(),values_.end())-values_.begin());
        const auto hz=frequencies_.size()==values_.size()?ui::text(" · %1 Hz").arg(frequencies_[at],0,'g',6):QString{};
        return ui::text("Peak: %1 · %2").arg(note_name(first_+at)).arg(peak_value(),0,'g',5)+hz;
    }
    void publish_presentation(){
        rebuild_keys();
        publish({{"kind","piano"},{"title",ui::text("Note activity")},{"status",values_.empty()&&status_.isEmpty()?ui::text("Waiting for note values"):status_},
            {"keys",keys_},{"levels",activity_levels()},{"peakText",peak_caption()},{"pianoRevision",qulonglong(revision_)},{"font",font()},
            {"whiteKey",palette().color(QPalette::Light)},{"blackKey",palette().color(QPalette::Dark)},{"keyText",palette().color(QPalette::Shadow)}});
    }
    QRectF keyboard_bounds() const{return {12,std::max(58.,height()*.52),double(std::max(1,width()-24)),std::max(20.,height()*.48-40)};}
    std::vector<float> values_;std::vector<double> frequencies_;QVariantList keys_;
    unsigned first_=21;double activity_floor_=1e-9;QString status_;bool layout_dirty_=true;quint64 revision_=0;
    visualization::rendering::QtFramePump pump_;
};
