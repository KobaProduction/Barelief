#pragma once
#include <barelief/classic_spectrogram_view.h>
#include <array>
#include <utility>
// Only the research layout and semantic axes differ from the classical view.
class CompactSpectrogramView final : public ClassicSpectrogramWidget {
public:
    explicit CompactSpectrogramView(QWidget* parent=nullptr):ClassicSpectrogramWidget(parent) {setObjectName("compactSpectrogramView");}
    bool set_layout(const QJsonObject&) override;
    bool accepts_columns(size_t count) const override {return count==96;}
    double frequency(size_t band) const override;
    QString note_label(size_t band) const;
    size_t section_count() const {return sections_.size();}
    QString section_label(size_t index) const;
    std::pair<int,int> section_range(size_t index) const;
protected:
    void draw_axes(QPainter&,const QRectF&) override;
    void draw_plot_overlay(QPainter&,const QRectF&) override;
    bool ready() const override {return boundaries_.size()==48;}
    double legend_offset() const override {return horizontal_?106:24;}
    int vertical_margin() const override {return 220;}
private:
    struct Section {int begin,end;QByteArray label;};
    bool set_sections(const QJsonArray&);
    std::pair<int,int> semantic_section(size_t,bool) const;
    std::vector<int> index_ticks() const;
    void draw_index_grid(QPainter&,const QRectF&);
    void draw_axis_frames(QPainter&,const QRectF&);
    void draw_range_ticks(QPainter&,const QRectF&,int,int,bool);
    void draw_band_tick(QPainter&,const QRectF&,bool,size_t);
    void draw_sections(QPainter&,const QRectF&);
    void draw_section_label(QPainter&,const QRectF&,const Section&,bool);
    void draw_group_ticks(QPainter&,const QRectF&,bool notes);
    void draw_index_axis(QPainter&,const QRectF&);
    std::vector<double> boundaries_;
    unsigned first_midi_=69;
    std::vector<Section> sections_;
};
