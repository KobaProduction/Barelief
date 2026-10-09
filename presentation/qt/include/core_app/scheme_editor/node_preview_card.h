#pragma once
#include "core_app/scheme_editor/audio_node_model.h"
#include "visualization/ui_text.h"
#include <QGridLayout>
#include <QPainter>
#include <QEvent>
#include <algorithm>

// Bounded, owned text/metadata only. No frame storage, DSP or parameter bindings.
class NodePreviewCard final : public QWidget {
public:
    void set_compact(bool compact) { compact_=compact;refresh(); }
    explicit NodePreviewCard(QWidget* parent=nullptr):QWidget(parent) {
        setObjectName("pipelineDataStatus");setMinimumWidth(0);
        auto* layout=new QGridLayout(this);layout->setContentsMargins(12,8,12,8);layout->setHorizontalSpacing(12);layout->setVerticalSpacing(3);
        status_=new QLabel(this);status_->setObjectName("previewStatus");status_->setWordWrap(true);
        auto font=status_->font();font.setBold(true);status_->setFont(font);layout->addWidget(status_,0,0,1,2);
        count_label_=new QLabel(this);count_label_->setWordWrap(true);layout->addWidget(count_label_,1,0);
        count_=new QLabel(this);count_->setObjectName("previewValues");count_->setAlignment(Qt::AlignRight);layout->addWidget(count_,1,1);
        value_label_=ui::make<QLabel>("First value",this);value_label_->setWordWrap(true);layout->addWidget(value_label_,2,0);
        value_=new QLabel(this);value_->setObjectName("previewExactValue");value_->setAlignment(Qt::AlignRight);value_->setWordWrap(true);layout->addWidget(value_,2,1);
        detail_=new QLabel(this);detail_->setObjectName("previewUnit");detail_->setWordWrap(true);layout->addWidget(detail_,3,0,1,2);layout->setColumnStretch(0,1);
        setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
        for(auto* label:{status_,count_label_,count_,value_label_,value_,detail_})label->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Preferred);
        // Keep an actual value column; Ignored made its size hint disappear.
        layout->setColumnStretch(1,1);
        connect(&ui::Language::instance(),&ui::Language::changed,this,[this]{refresh();});
        connect(&ui::Theme::instance(),&ui::Theme::changed,this,[this]{refresh();});refresh();
    }
    void set_data(const std::vector<AudioNodeModel::DiagnosticPreview>& values,bool enabled,const QString& activity={}) {values_=values;enabled_=enabled;activity_=activity;setProperty("previewActivity",activity);refresh();}
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);
        const bool available=activity_.isEmpty() && std::any_of(values_.begin(),values_.end(),[](const auto& value){return value.available;});
        painter.setPen(QPen(palette().color(available?QPalette::Highlight:QPalette::Mid),1));painter.setBrush(palette().color(QPalette::AlternateBase));
        painter.drawRoundedRect(QRectF(rect()).adjusted(.5,.5,-.5,-.5),5,5);
    }
    void changeEvent(QEvent* event) override {QWidget::changeEvent(event);if(event->type()==QEvent::PaletteChange)refresh();}
private:
    QLabel *status_,*count_label_,*count_,*value_label_,*value_,*detail_;
    bool enabled_=true,compact_=false;
    QString activity_;
    std::vector<AudioNodeModel::DiagnosticPreview> values_;
    void refresh() {
        QStringList statuses,labels,counts,values,details;
        for(const auto& preview:values_) {
            const auto port=ui::text(preview.direction==domain::PortDirection::Output?"Output port %1":"Input port %1").arg(preview.port);
            const bool available=activity_.isEmpty() && preview.available;
            statuses.append(port+" · "+(enabled_?QString{}:ui::text("Bypass")+" · ")+ui::text(!activity_.isEmpty()?activity_.toUtf8().constData():!preview.failure.isEmpty()?preview.failure.toUtf8().constData():available?"Available":preview.sequence_seen?"Unavailable":"Waiting for data"));
            labels.append(ui::text(preview.count_source.toUtf8().constData()));counts.append(available?QString::number(preview.count):QStringLiteral("—"));
            values.append(available && !preview.exact_value.isEmpty()?preview.exact_value:QStringLiteral("—"));details.append(port+" · "+preview.description+(preview.sequence_seen?"\n"+ui::text("Frame %1; timestamp %2 ticks").arg(preview.sequence).arg(preview.timestamp_ticks):QString{}));
        }
        status_->setText(statuses.isEmpty()?(enabled_?QString{}:ui::text("Bypass")+" · ")+ui::text("Waiting for data"):statuses.join('\n'));
        count_label_->setText(labels.isEmpty()?ui::text("Values"):labels.join('\n'));count_->setText(counts.isEmpty()?QStringLiteral("—"):counts.join('\n'));
        value_->setText(values.isEmpty()?QStringLiteral("—"):values.join('\n'));detail_->setText(details.join('\n'));
        const bool show_values=!compact_ || (activity_.isEmpty() && std::any_of(values_.begin(),values_.end(),[](const auto& p){return p.available;}));
        for(auto* label:{count_label_,count_,value_label_,value_})label->setVisible(show_values);
        detail_->setVisible(!compact_);
        updateGeometry();
        setAccessibleName(ui::text("Data status"));setAccessibleDescription(status_->text()+", "+count_label_->text()+": "+count_->text()+", "+value_label_->text()+": "+value_->text());update();
    }
};
