#pragma once
#include <functional>
#include <QStringList>
#include <visualization/numeric_matrix_snapshot.h>
#include <visualization/spectrogram_axis_metadata.h>
namespace presentation {
// Synchronous GUI projection ports: callbacks borrow values only during delivery.
// Immutable matrices may be retained by a renderer; no processing FrameView crosses here.
struct NumericCaptionPort {
    std::function<void(const char*,QStringList,bool,bool)> caption;
};
struct NumericChartPort : NumericCaptionPort {
    std::function<void(const std::vector<float>&)> update;
    std::function<void(const QString&,bool,bool)> set_scalar_readout;
    std::function<bool()> scalar_readout_enabled;
    std::function<bool(const std::vector<double>&)> set_x_coordinates;
    std::function<void(const QString&,const QString&)> set_axis_title_keys;
};
struct NumericSurfacePort : NumericCaptionPort {
    std::function<void(const std::vector<std::vector<float>>&)> update_frame_history;
    std::function<bool()> paused;
    std::function<void(size_t,size_t)> set_history_dimensions;
    std::function<bool(const SpectrogramAxisMetadata&)> set_axis_metadata;
    std::function<void(NumericMatrixSnapshot)> update_snapshot;
};
struct NumericImagePort : NumericCaptionPort {
    std::function<void(const std::vector<std::vector<float>>&)> set_matrix;
    std::function<bool(const SpectrogramAxisMetadata&)> set_axis_metadata;
    std::function<void(NumericMatrixSnapshot,bool)> set_snapshot;
};
struct NumericPiePort : NumericCaptionPort {
    std::function<bool(const std::vector<float>&)> set_values;
};
struct NumericMetricsPort {
    std::function<void()> clear_values;
    std::function<void(quint16)> set_unavailable;
    std::function<void(quint16,const QString&)> set_value;
};
// Physical units require explicit configuration links at the composition boundary.
struct NumericAxisPermissions { bool frequency=false; bool time=false; };
struct NumericRenderTarget {
    NumericChartPort* chart=nullptr;
    NumericSurfacePort* spectrogram=nullptr;
    NumericCaptionPort* exact_label=nullptr;
    bool visible=false;
    NumericImagePort* image=nullptr;
    NumericPiePort* pie=nullptr;
    NumericMetricsPort* metrics=nullptr;
    NumericAxisPermissions axes;
};
}

