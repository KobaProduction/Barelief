#pragma once
#include <visualization/abstract/abs.h>
#include <visualization/spectrograms/spectrogram_graph.h>
#include <visualization/numeric_image_view.h>
#include <visualization/numeric_pie_view.h>
#include <visualization/numeric_metrics_view.h>
#include <visualization/quick/numeric_render_ports.h>
#include <visualization/ui_text.h>
#include <QLabel>

namespace render_fixture {
// Test-side composition of real QML renderers with the production projection
// ports. No widget knowledge enters NumericViewAdapter through this fixture.
struct Target {
    presentation::NumericChartPort chartPort;
    presentation::NumericSurfacePort surfacePort;
    presentation::NumericImagePort imagePort;
    presentation::NumericPiePort piePort;
    presentation::NumericMetricsPort metricsPort;
    presentation::NumericCaptionPort captionPort;
    bool visible=false;
    bool chartEnabled=false,surfaceEnabled=false,imageEnabled=false,pieEnabled=false,metricsEnabled=false,captionEnabled=false;
    static QString text(const char* source,const QStringList& args,bool loss,bool unavailable) {
        auto result=*source?ui::text(source):QString{};
        for(const auto& arg:args)result=result.arg(arg);
        if(loss)result+=(result.isEmpty()?QString{}:QStringLiteral(" · "))+ui::text("Renderer precision loss");
        if(unavailable)result+=(result.isEmpty()?QString{}:QStringLiteral(" · "))+ui::text("Renderer projection unavailable");
        return result;
    }
    Target(AbstractChartView* chart=nullptr,SpectrogramGraph* surface=nullptr,QLabel* caption=nullptr,bool exposed=false,
           NumericImageView* image=nullptr,NumericPieView* pie=nullptr,NumericMetricsView* metrics=nullptr):visible(exposed) {
        if(chart) {
            chartEnabled=true;
            chartPort.update=[chart](const auto& values){chart->update(values);};
            chartPort.set_scalar_readout=[chart](const auto& value,bool scalar,bool available){chart->set_scalar_readout(value,scalar,available);};
            chartPort.scalar_readout_enabled=[chart]{return chart->scalar_readout_enabled();};
            chartPort.set_x_coordinates=[chart](const auto& values){return chart->set_x_coordinates(values);};
            chartPort.set_axis_title_keys=[chart](const auto& x,const auto& y){chart->set_axis_titles(ui::text(x.toUtf8().constData()),ui::text(y.toUtf8().constData()));};
            chartPort.caption=[chart](const char* source,QStringList args,bool loss,bool unavailable){chart->set_projection_status(text(source,args,loss,unavailable));};
        }
        if(surface) {
            surfaceEnabled=true;
            surfacePort.update_frame_history=[surface](const auto& values){surface->update_frame_history(values);};
            surfacePort.paused=[surface]{return surface->paused();};
            surfacePort.set_history_dimensions=[surface](size_t rows,size_t columns){surface->set_history_dimensions(rows,columns);};
            surfacePort.set_axis_metadata=[surface](const auto& axes){return surface->set_axis_metadata(axes);};
            surfacePort.update_snapshot=[surface](auto snapshot){surface->update_snapshot(std::move(snapshot));};
            surfacePort.caption=[surface](const char* source,QStringList args,bool loss,bool unavailable){surface->set_projection_status(text(source,args,loss,unavailable));};
        }
        if(image) {
            imageEnabled=true;
            imagePort.set_matrix=[image](const auto& values){image->set_matrix(values);};
            imagePort.set_axis_metadata=[image](const auto& axes){return image->set_axis_metadata(axes);};
            imagePort.set_snapshot=[image](auto snapshot,bool temporal){image->set_snapshot(std::move(snapshot),temporal);};
            imagePort.caption=[image](const char* source,QStringList args,bool loss,bool unavailable){image->set_projection_status(text(source,args,loss,unavailable));};
        }
        if(pie) {
            pieEnabled=true;
            piePort.set_values=[pie](const auto& values){return pie->set_values(values);};
            piePort.caption=[pie](const char* source,QStringList args,bool loss,bool unavailable){pie->set_projection_status(text(source,args,loss,unavailable));};
        }
        if(metrics) {
            metricsEnabled=true;
            metricsPort.clear_values=[metrics]{metrics->clear_values();};
            metricsPort.set_unavailable=[metrics](quint16 port){metrics->set_unavailable(port);};
            metricsPort.set_value=[metrics](quint16 port,const auto& value){metrics->set_value(port,value);};
        }
        if(caption) {
            captionEnabled=true;
            captionPort.caption=[caption](const char* source,QStringList args,bool loss,bool unavailable){
                caption->setProperty("numericCaptionLoss",loss);caption->setProperty("numericCaptionUnavailable",unavailable);
                caption->setText(text(source,args,loss,unavailable));
            };
        }
    }
    operator presentation::NumericRenderTarget() {
        return {chartEnabled?&chartPort:nullptr,surfaceEnabled?&surfacePort:nullptr,captionEnabled?&captionPort:nullptr,visible,
            imageEnabled?&imagePort:nullptr,pieEnabled?&piePort:nullptr,metricsEnabled?&metricsPort:nullptr};
    }
};
inline QVariantList points(const AbstractChartView& chart){return chart.presentation_model()->state().value("points").toList();}
}
