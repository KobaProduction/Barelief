#pragma once
#include <core_app/graph_panel.h>
#include <signal_contracts/data_contract.h>
#include <QJsonArray>
namespace presentation {
struct PlotDescriptor {
    QStringList modes{"line","points","bars","pie"};
    QStringList category_path{"Graphs","Universal","Vectors"};
    bool audio_scales=false;
};
PlotDescriptor plot_descriptor_from_preferences(const QJsonObject&,const PlotDescriptor& fallback);
QJsonObject plot_descriptor_preferences(const PlotDescriptor&);
PlotCapabilities plot_capabilities(const PlotDescriptor&);
PlotPresentation audio_plot_presentation(const signal_contracts::NumericContract&);
QStringList plot_category_path(const PlotDescriptor&);
QString explicit_plot_sections(const QJsonArray& sections);
}
