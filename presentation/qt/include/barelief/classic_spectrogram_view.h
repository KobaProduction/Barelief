#pragma once
#include <barelief/numeric_matrix_snapshot.h>
#include <barelief/spectrogram_axis_metadata.h>
#include <QWidget>
#include <QImage>
#include <QJsonObject>
#include <QElapsedTimer>
#include <QPen>
#include <QStringList>

class QTimer;

class QPainter;
// Shared classical matrix renderer: palette, history, direction and physical axes.
// Borrows one immutable GUI snapshot; the processing history remains UInt8.
class ClassicSpectrogramWidget : public QWidget {
    Q_OBJECT
public:
    explicit ClassicSpectrogramWidget(QWidget* parent=nullptr);
    virtual bool accepts_columns(size_t count) const;
    void set_orientation(const QString&);
    virtual bool set_layout(const QJsonObject&) {return true;}
    void set_snapshot(NumericMatrixSnapshot,bool age_order);
    bool set_axis_metadata(const SpectrogramAxisMetadata&);
    void set_projection_status(const QString&);
    void set_value_range(double minimum,double maximum);
    void set_render_visible(bool visible);
    qreal display_fps() const;
    const void* projection_storage_identity() const {return data_.storage_identity();}
    const SpectrogramAxisMetadata& source_axis_metadata() const {return axes_;}
    virtual double frequency(size_t band) const;
    double age(size_t row) const;
    bool sample(size_t row,size_t column,float& value) const;
    QPair<double,double> value_range() const {return {minimum_,maximum_};}
    size_t value_count() const {return data_.sample_count();}
    float peak_value() const;
signals:
    void axis_metadata_changed();
protected:
    void paintEvent(QPaintEvent*) override;
    void changeEvent(QEvent*) override;
    void resizeEvent(QResizeEvent*) override;
protected:
    QRectF plot_rect(int group) const;
    void rebuild_raster();
    void invalidate_raster();
    void prepare_raster(size_t frames,size_t bands);
    void reduce_raster(size_t frames,size_t bands);
    void color_raster(size_t bands);
    void invalidate_axes();
    QStringList axis_labels() const;
    void rebuild_axes();
    void paint_axes_layer();
    void draw_group(QPainter&,int group);
    void draw_classical_axes(QPainter&,const QRectF&);
    void draw_horizontal_time_axis(QPainter&,const QRectF&);
    virtual void draw_axes(QPainter&,const QRectF&);
    virtual void draw_plot_overlay(QPainter&,const QRectF&);
    virtual bool ready() const {return true;}
    virtual double legend_offset() const {return 24;}
    virtual int vertical_margin() const {return 180;}
    void draw_time_axis(QPainter&,const QRectF&);
    void draw_value_scale(QPainter&);
    QPen axis_pen() const;
    void sample_frame_rate();
    NumericMatrixSnapshot data_;
    SpectrogramAxisMetadata axes_;
    QImage raster_;
    std::vector<float> raster_peaks_;
    std::vector<size_t> raster_columns_;
    size_t raster_bands_=0;
    quint64 raster_revision_=0;
    bool raster_dirty_=true;
    QImage axis_layer_;
    QStringList axis_labels_;
    quint64 axis_revision_=0;
    bool axis_dirty_=true;
    QString status_;
    bool horizontal_=false;
    double minimum_=0,maximum_=255;
    bool age_order_=true,render_visible_=true;
    QElapsedTimer fps_clock_;
    QTimer* fps_timer_=nullptr;
    quint64 painted_frames_=0,sampled_frames_=0;
    qreal measured_fps_=0;
};
