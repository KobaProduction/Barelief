#pragma once
#include <dsp_kernels/frequency_profile.h>
#include <barelief/ui_text.h>
#include <QAbstractTableModel>
#include <memory>

namespace core_app {
// Virtual rows keep a large union of native grids out of QTableWidget cells.
class MusicFrequencyModel final : public QAbstractTableModel {
    std::shared_ptr<const dsp_kernels::FrequencyProfile> profile_;
    bool explicit_axis()const {return profile_ && profile_->curve && !profile_->curve->measurement_hz.empty();}
    std::size_t size()const {return !profile_?0:explicit_axis()?profile_->curve->measurement_hz.size():profile_->gains.size();}
    double hz(std::size_t i)const {
        return explicit_axis()?profile_->curve->measurement_hz[i]:i*profile_->compatibility.sample_rate/profile_->compatibility.fft_size;
    }
    double measured(std::size_t i)const {
        if(explicit_axis())return profile_->curve->measured_spectrum[i];
        return profile_->peak_spectrum.empty()?profile_->mean_spectrum[i]:profile_->peak_spectrum[i];
    }
    double gain(std::size_t i)const {return profile_->curve?dsp_kernels::frequency_curve_gain(*profile_,hz(i)):profile_->gains[i];}
public:
    explicit MusicFrequencyModel(QObject* parent):QAbstractTableModel(parent){}
    void set_profile(std::shared_ptr<const dsp_kernels::FrequencyProfile> profile) {
        beginResetModel();profile_=std::move(profile);endResetModel();
    }
    int rowCount(const QModelIndex& parent={})const override {return parent.isValid()?0:int(size()?size()-1:0);}
    int columnCount(const QModelIndex& parent={})const override {return parent.isValid()?0:6;}
    QVariant data(const QModelIndex& index,int role=Qt::DisplayRole)const override {
        if(role!=Qt::DisplayRole || !index.isValid() || index.row()<0 || index.row()>=rowCount())return {};
        const auto i=std::size_t(index.row()+1);
        switch(index.column()){
            case 0:return QString::number(i);
            case 1:return QString::number(hz(i),'g',10);
            case 2:return QString::number(measured(i),'g',10);
            case 3:return QString::number(gain(i),'g',9);
            case 4:return QString::number(measured(i)*gain(i),'g',10);
            case 5:if(explicit_axis())return QString::number(profile_->curve->contributors[i]);
                return profile_->frequency_presence.empty()?QString{}:QString::number(100*profile_->frequency_presence[i],'f',2);
            default:return {};
        }
    }
    QVariant headerData(int section,Qt::Orientation orientation,int role=Qt::DisplayRole)const override {
        if(role!=Qt::DisplayRole)return {};
        if(orientation==Qt::Vertical)return QString::number(section+1);
        const char* titles[]{"Point","Frequency, Hz","Measured amplitude","Linear gain","Calculated corrected amplitude","Contributing tracks"};
        if(section<0 || section>=6)return {};
        return ui::text(section==5 && !explicit_axis()?"Presence, %":titles[section]);
    }
};
}
