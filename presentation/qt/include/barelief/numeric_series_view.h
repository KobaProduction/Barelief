#pragma once
#include <barelief/quick2d/plot_model.h>
#include <barelief/value_display_scale.h>
class QPainter;
class QPainterPath;

// General numeric comparison plot. Coordinates and display units are explicit;
// this renderer has no knowledge of audio or processing graph topology.
class NumericSeriesView : public QuickPlotWidget {
public:
    struct Series {QString name;std::vector<double> values;bool dashed=false;};
    struct Band {double first=0,last=0;bool emphasized=false;};
    // Nestable synchronous transaction: publish only the final presentation.
    class BatchUpdate final {
    public:
        explicit BatchUpdate(NumericSeriesView& view):view_(view){++view_.update_depth_;}
        ~BatchUpdate(){if(--view_.update_depth_==0 && view_.refresh_pending_)view_.refresh_presentation();}
        BatchUpdate(const BatchUpdate&)=delete;
        BatchUpdate& operator=(const BatchUpdate&)=delete;
    private:
        NumericSeriesView& view_;
    };
    BatchUpdate batch_update(){return BatchUpdate(*this);}
    explicit NumericSeriesView(QWidget* parent=nullptr);
    void set_series(std::vector<double> coordinates,std::vector<Series> series);
    void set_scale(ValueDisplayScale scale,bool automatic,double low,double high);
    void set_titles(QString x,QString y);
    void set_logarithmic_x(bool enabled);
    void set_bands(std::vector<Band> bands);
    void set_horizontal_view(double left,double width);
    std::pair<double,double> x_range()const;
    std::pair<double,double> y_range()const{return {low_,high_};}
protected:
    void refresh_presentation() override;
    QPainterPath series_path(const Series&,const QSize&,double,double)const;
private:
    std::vector<double> coordinates_;
    std::vector<Series> series_;
    std::vector<Band> bands_;
    ValueDisplayScale scale_;
    QString x_title_,y_title_;
    bool automatic_=true,logarithmic_=false;
    bool range_dirty_=true,refresh_pending_=false;
    unsigned update_depth_=0;
    std::pair<double,double> observed_range_;
    double low_=-120,high_=20;
    double map_x(double value)const;
    double coordinate(double fraction)const;
    void observe_range();
    std::pair<double,double> data_range()const;
    std::pair<std::size_t,std::size_t> visible_indices(std::size_t,double,double)const;
    QImage raster(double minimum,double maximum)const;
    void draw_series(QPainter&,const QSize&,double,double)const;
    void draw_bands(QPainter&,const QSize&,double,double)const;
    void hover(double x,double y);
};
