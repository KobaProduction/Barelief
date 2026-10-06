#pragma once

#include "pipeline/graph.h"
#include "visualization/ui_text.h"
#include <QApplication>
#include <QGridLayout>
#include <QLabel>
#include <QPainter>
#include <QEvent>
#include <algorithm>
#include <cmath>

// Read-only telemetry. This widget has no parameter bindings or JSON writer.
class NodePreviewCard final : public QWidget {
public:
    explicit NodePreviewCard(QWidget *parent=nullptr) : QWidget(parent) {
        setObjectName("pipelineDataStatus");
        setMinimumWidth(0);
        auto *layout=new QGridLayout(this);
        layout->setContentsMargins(12,8,12,8);
        layout->setHorizontalSpacing(12);
        layout->setVerticalSpacing(3);
        _status=new QLabel(this); _status->setObjectName("previewStatus");
        _status->setWordWrap(true);
        auto font=_status->font(); font.setBold(true); _status->setFont(font);
        layout->addWidget(_status,0,0,1,2);
        _count_label=new QLabel(this); layout->addWidget(_count_label,1,0);
        _count_label->setWordWrap(true);
        _count=new QLabel(this); _count->setObjectName("previewValues");
        _count->setAlignment(Qt::AlignRight); layout->addWidget(_count,1,1);
        auto *peak_label=ui::make<QLabel>("Peak",this); peak_label->setWordWrap(true);
        layout->addWidget(peak_label,2,0);
        _peak=new QLabel(this); _peak->setObjectName("previewPeak");
        _peak->setAlignment(Qt::AlignRight); layout->addWidget(_peak,2,1);
        _unit=new QLabel(this); _unit->setObjectName("previewUnit");
        _unit->setWordWrap(true);
        QSizePolicy wrapping(QSizePolicy::Ignored,QSizePolicy::Preferred);
        wrapping.setHeightForWidth(true);
        _unit->setSizePolicy(wrapping);
        _count_label->setSizePolicy(wrapping);
        layout->setColumnStretch(0,1);
        auto small=_unit->font(); small.setPointSizeF(std::max(7.0,small.pointSizeF()-1)); _unit->setFont(small);
        layout->addWidget(_unit,3,0,1,2);
        connect(&ui::Language::instance(),&ui::Language::changed,this,[this] { refresh(); });
        connect(&ui::Theme::instance(),&ui::Theme::changed,this,[this] { refresh(); });
        refresh();
    }
    void set_data(pipeline::NodePreview value, bool enabled, const QString &type) {
        _value=value; _enabled=enabled; _type=type; refresh();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(palette().color(_enabled && _value.values ? QPalette::Highlight : QPalette::Mid),1));
        painter.setBrush(palette().color(QPalette::AlternateBase));
        painter.drawRoundedRect(QRectF(rect()).adjusted(0.5,0.5,-0.5,-0.5),5,5);
    }
    void changeEvent(QEvent *event) override {
        QWidget::changeEvent(event);
        if(event->type()==QEvent::PaletteChange) { refresh(); update(); }
    }
private:
    QLabel *_status,*_count_label,*_count,*_peak,*_unit;
    pipeline::NodePreview _value;
    bool _enabled=true;
    QString _type;
    void refresh() {
        const bool available=_enabled && _value.values>0;
        _status->setText(ui::text(!_enabled ? "Output disabled" : available ? "Available" : "Waiting for data"));
        const bool audio=_type=="audio" || _type=="window" || _type=="windowed";
        const bool spectrum=_type=="spectrum" || _type=="stereo-spectrum";
        _count_label->setText(ui::text(audio ? "Samples" : spectrum ? "Bins" : _type=="mel" ? "Bands" : "Values"));
        _count->setText(available ? QString::number(_value.values) : QStringLiteral("—"));
        _peak->setText(available && std::isfinite(_value.peak) ? QString::number(_value.peak,'g',3) : QStringLiteral("—"));
        _unit->setText(ui::text(audio ? "Normalized amplitude" : "Magnitude"));
        setAccessibleName(ui::text("Data status"));
        setAccessibleDescription(_status->text()+", "+_count_label->text()+": "+_count->text()+", "+ui::text("Peak")+": "+_peak->text());
        update();
    }
};
