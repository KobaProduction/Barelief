#pragma once
#include <offline/resampler_comparison.h>
#include <functional>
class QWidget;
using ResamplerComparisonService=std::function<std::vector<desktop_analysis::offline::ComparisonResult>(const desktop_analysis::offline::ComparisonRequest&,const std::atomic_bool&)>;
void show_resampler_comparison(QWidget* parent,ResamplerComparisonService service={});
