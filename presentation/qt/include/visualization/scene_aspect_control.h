#pragma once
#include <QWidget>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <functional>
#include <visualization/surface_settings.h>
#include <visualization/ui_text.h>

// Shared presentation control: it changes scene proportions, never signal gain,
// cropping, physical coordinates, camera state or the persisted axis ranges.
namespace ui {
class SceneAspectControl final : public QWidget {
public:
    explicit SceneAspectControl(QWidget* parent=nullptr):QWidget(parent) {
        auto* form=new QFormLayout(this);form->setContentsMargins(0,0,0,0);
        form->setRowWrapPolicy(QFormLayout::WrapLongRows);
        build_editors();
        add_row(*form,"Scene proportions",mode);add_row(*form,"Time width / height",time);add_row(*form,"Band depth / height",band);
        bind_changes();set_options({});
    }
private:
    void build_editors() {
        mode=new QComboBox(this);mode->setObjectName("surface_scene_aspect_mode");
        ui::items(mode,{"Fixed scene proportions","Proportions from sample counts"});
        mode->setMinimumWidth(0);mode->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        mode->setMinimumContentsLength(8);
        time=number("surface_scene_time_aspect");band=number("surface_scene_band_aspect");
    }
    void add_row(QFormLayout& form,const char* title,QWidget* editor) {
        auto* label=ui::make<QLabel>(title,this);label->setWordWrap(true);form.addRow(label,editor);
    }
    void bind_changes() {
        connect(mode,&QComboBox::currentIndexChanged,this,[this]{syncEnabled();if(changed)changed();});
        for(auto* spin:{time,band})
            connect(spin,&QDoubleSpinBox::valueChanged,this,[this]{if(changed)changed();});
    }
public:
    void set_options(const QJsonObject& raw) {
        const auto value=SurfaceSettings::normalized(raw);
        const QSignalBlocker m(mode),t(time),b(band);
        mode->setCurrentIndex(value["scene_aspect_mode"]=="sample_counts"?1:0);
        time->setValue(value["scene_time_aspect"].toDouble());band->setValue(value["scene_band_aspect"].toDouble());
        syncEnabled();
    }
    QJsonObject apply(QJsonObject options) const {
        const auto previous=SurfaceSettings::normalized(options);
        options["scene_aspect_mode"]=mode->currentIndex()==1?"sample_counts":"fixed";
        // Keep exact stored ratios when another control is edited.
        const auto ratio=[&](const char* key,QDoubleSpinBox* spin) {
            const auto old=previous[key].toDouble();
            return std::round(old*100)==std::round(spin->value()*100)?old:spin->value();
        };
        options["scene_time_aspect"]=ratio("scene_time_aspect",time);
        options["scene_band_aspect"]=ratio("scene_band_aspect",band);
        return SurfaceSettings::normalized(options);
    }
    std::function<void()> changed;
private:
    QComboBox* mode=nullptr;
    QDoubleSpinBox *time=nullptr,*band=nullptr;
    QDoubleSpinBox* number(const char* name) {
        auto* spin=new QDoubleSpinBox(this);spin->setObjectName(name);
        spin->setRange(.25,4);spin->setDecimals(2);spin->setSingleStep(.05);return spin;
    }
    void syncEnabled(){time->setEnabled(mode->currentIndex()==0);band->setEnabled(mode->currentIndex()==0);}
};
}
