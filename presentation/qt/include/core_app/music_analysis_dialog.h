#pragma once
#include <QDialog>
#include <memory>
#include <core_app/frequency_profile_binding.h>
#include <functional>
namespace core_app {
class MusicAnalysisDialog final : public QDialog {
 public:
    explicit MusicAnalysisDialog(QWidget* parent = nullptr);
    using Targets=std::function<std::vector<FrequencyProfileTarget>()>;
    using Apply=std::function<bool(const FrequencyProfileTarget&,const dsp_kernels::FrequencyProfile&,QString&)>;
    MusicAnalysisDialog(Targets,Apply,desktop_analysis::offline::AnalysisServices,QWidget* parent=nullptr);
    ~MusicAnalysisDialog() override;
    bool save_profile_to(const QString& path,QString& error) const;
    bool load_profile_from(const QString& path,QString& error);
    bool save_scan_to(const QString& path,QString& error) const;
    bool load_scan_from(const QString& path,QString& error);
 private:
    struct State;
    std::unique_ptr<State> state_;
};
void show_music_analysis(QWidget* parent);
}
