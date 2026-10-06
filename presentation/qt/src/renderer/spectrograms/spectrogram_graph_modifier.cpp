#include "visualization/ui_text.h"
#include "core/performance.h"
#include "visualization/spectrograms/spectrogram_graph_modifier.h"
#include <algorithm>
#include <QApplication>

#include <QtDataVisualization/qvalue3daxis.h>
#include <QtDataVisualization/q3dtheme.h>

#include <QtGui/qquaternion.h>

#include <QtCore/qmath.h>

using namespace Qt::StringLiterals;

SpectrogramGraphModifier::SpectrogramGraphModifier(Q3DSurface *spectrogram, QLabel *label, QObject *parent) : QObject(parent),
    m_graph(spectrogram),
    m_textField(label) {
    m_graph->scene()->activeCamera()->setZoomLevel(155.f);
    m_graph->scene()->activeCamera()->setCameraPreset(Q3DCamera::CameraPresetIsometricRightHigh);
    auto update_theme=[this] {
        m_graph->activeTheme()->setType(QApplication::palette().color(QPalette::Window).lightness()<128 ? Q3DTheme::ThemeEbony : Q3DTheme::ThemeArmyBlue);
    };
    update_theme(); connect(&ui::Theme::instance(),&ui::Theme::changed,this,update_theme);
    m_graph->setShadowQuality(QAbstract3DGraph::ShadowQualityNone);

    m_graph->setAxisX(new QValue3DAxis);
    m_graph->setAxisY(new QValue3DAxis);
    m_graph->setAxisZ(new QValue3DAxis);

    m_dataProxy = new QSurfaceDataProxy();
    m_dataSeries = new QSurface3DSeries(m_dataProxy);

    fillDataProxy();

    connect(m_graph, &QAbstract3DGraph::selectedElementChanged,
            this, &SpectrogramGraphModifier::handleElementSelected);

    m_selectionAnimation = new QPropertyAnimation(this);
    m_selectionAnimation->setPropertyName("scaling");
    m_selectionAnimation->setDuration(100);
    m_selectionAnimation->setLoopCount(-1);
}

void SpectrogramGraphModifier::update(const std::vector<std::vector<float> > &spectrogram) {
    core::perf::Scope trace(core::perf::Stage::SpectrogramGeometry);
    if (spectrogram.size() < 2 || spectrogram[0].size() < 2) return;
    if (std::any_of(spectrogram.begin(), spectrogram.end(), [&](const auto &row) { return row.size() != spectrogram[0].size(); })) return;
    if (spectrogram.size() not_eq sampleCountZ or spectrogram[0].size() not_eq sampleCountX) {

        updateAxisRanges(0, spectrogram[0].size(), 0, spectrogram.size());
    }

    const auto *existing = m_dataProxy->array();
    if (existing && existing->size() == spectrogram.size() && !existing->empty() && existing->first()->size() == spectrogram[0].size()) {
        // Reuse proxy rows; updating values avoids rebuilding 20,000 Qt items every frame.
        for (int ch = 0; ch < spectrogram.size(); ++ch) {
            auto *row = existing->at(ch);
            for (int x = 0; x < row->size(); ++x) (*row)[x].setY(spectrogram[ch][x]);
        }
        emit m_dataProxy->rowsChanged(0, spectrogram.size());
    } else {
        auto *dataArray = new QSurfaceDataArray;
        dataArray->reserve(spectrogram.size());
        for (int ch = 0; ch < spectrogram.size(); ++ch) {
            auto *row = new QSurfaceDataRow(spectrogram[ch].size());
            for (int x = 0; x < row->size(); ++x) (*row)[x].setPosition({float(x), spectrogram[ch][x], float(ch)});
            dataArray->append(row);
        }
        m_dataProxy->resetArray(dataArray);
    }
}


void SpectrogramGraphModifier::fillDataProxy() {
    float stepX = (sampleMaxX - sampleMinX) / float(sampleCountX - 1);
    float stepZ = (sampleMaxX - sampleMinX) / float(sampleCountZ - 1);

    auto *dataArray = new QSurfaceDataArray;
    dataArray->reserve(sampleCountZ);
    for (int i = 0; i < sampleCountZ; ++i) {
        auto *newRow = new QSurfaceDataRow;
        newRow->reserve(sampleCountX);
        // Keep values within range bounds, since just adding step can cause minor drift due
        // to the rounding errors.
        float z = qMin(sampleMaxZ, (i * stepZ + sampleMinZ));
        for (int j = 0; j < sampleCountX; ++j) {
            float x = qMin(sampleMaxX, (j * stepX + sampleMinX));
            float R = qSqrt(z * z + x * x) + 0.01f;
            float y = (qSin(R) / R + 0.24f) * 1.61f;
            newRow->append(QSurfaceDataItem({
                x, y, z
            }));
        }
        dataArray->append(newRow);
    }

    m_dataProxy->resetArray(dataArray);
}

void SpectrogramGraphModifier::enable3DModel() {
    m_dataSeries->setDrawMode(QSurface3DSeries::DrawSurfaceAndWireframe);
    // Flat shading expands the dynamic mesh and recomputes per-face normals every frame.
    m_dataSeries->setFlatShadingEnabled(false);

    m_graph->axisX()->setLabelFormat("%.2f");
    m_graph->axisZ()->setLabelFormat("%.2f");
    m_graph->axisX()->setRange(sampleMinX, sampleMaxX);
    m_graph->axisY()->setRange(0.f, 2.f);
    m_graph->axisZ()->setRange(sampleMinZ, sampleMaxZ);
    m_graph->axisX()->setLabelAutoRotation(30.f);
    m_graph->axisY()->setLabelAutoRotation(90.f);
    m_graph->axisZ()->setLabelAutoRotation(30.f);

    m_graph->axisX()->setReversed(true);

    m_graph->addSeries(m_dataSeries);

    m_graph->axisX()->setTitleVisible(true);
    m_graph->axisY()->setTitleVisible(true);
    m_graph->axisZ()->setTitleVisible(true);

    ui::bind(m_graph->axisX(),"title","Time (x)");
    ui::bind(m_graph->axisY(),"title","Signal (y)");
    ui::bind(m_graph->axisZ(),"title","Frequency (z)");

    m_graph->setActiveInputHandler(m_defaultInputHandler);

    m_rangeMinX = sampleMinX;
    m_rangeMinZ = sampleMinZ;
    m_stepX = (sampleMaxX - sampleMinX) / float(sampleCountX - 1);
    m_stepZ = (sampleMaxZ - sampleMinZ) / float(sampleCountZ - 1);
    m_axisMinSliderX->setMinimum(0);
    m_axisMinSliderX->setMaximum(sampleCountX - 2);
    m_axisMinSliderX->setValue(0);
    m_axisMaxSliderX->setMinimum(1);
    m_axisMaxSliderX->setMaximum(sampleCountX - 1);
    m_axisMaxSliderX->setValue(sampleCountX - 1);
    m_axisMinSliderZ->setMinimum(0);
    m_axisMinSliderZ->setMaximum(sampleCountZ - 2);
    m_axisMinSliderZ->setValue(0);
    m_axisMaxSliderZ->setMinimum(1);
    m_axisMaxSliderZ->setMaximum(sampleCountZ - 1);
    m_axisMaxSliderZ->setValue(sampleCountZ - 1);
}


void SpectrogramGraphModifier::adjustXMin(int min) {
    float minX = m_stepX * float(min) + m_rangeMinX;

    int max = m_axisMaxSliderX->value();
    if (min >= max) {
        max = min + 1;
        m_axisMaxSliderX->setValue(max);
    }
    float maxX = m_stepX * max + m_rangeMinX;

    setAxisXRange(minX, maxX);
}

void SpectrogramGraphModifier::adjustXMax(int max) {
    float maxX = m_stepX * float(max) + m_rangeMinX;

    int min = m_axisMinSliderX->value();
    if (max <= min) {
        min = max - 1;
        m_axisMinSliderX->setValue(min);
    }
    float minX = m_stepX * min + m_rangeMinX;

    setAxisXRange(minX, maxX);
}

void SpectrogramGraphModifier::adjustZMin(int min) {
    float minZ = m_stepZ * float(min) + m_rangeMinZ;

    int max = m_axisMaxSliderZ->value();
    if (min >= max) {
        max = min + 1;
        m_axisMaxSliderZ->setValue(max);
    }
    float maxZ = m_stepZ * max + m_rangeMinZ;

    setAxisZRange(minZ, maxZ);
}

void SpectrogramGraphModifier::adjustZMax(int max) {
    float maxX = m_stepZ * float(max) + m_rangeMinZ;

    int min = m_axisMinSliderZ->value();
    if (max <= min) {
        min = max - 1;
        m_axisMinSliderZ->setValue(min);
    }
    float minX = m_stepZ * min + m_rangeMinZ;

    setAxisZRange(minX, maxX);
}

void SpectrogramGraphModifier::updateAxisRanges(float xMin, float xMax, float zMin, float zMax) {
    sampleCountX = xMax;
    sampleCountZ = zMax;
    sampleMinX = xMin;
    sampleMaxX = xMax;
    sampleMinZ = zMin;
    sampleMaxZ = zMax;
    enable3DModel();
}


void SpectrogramGraphModifier::setAxisXRange(float min, float max) {
    m_graph->axisX()->setRange(min, max);
}

void SpectrogramGraphModifier::setAxisZRange(float min, float max) {
    m_graph->axisZ()->setRange(min, max);
}

void SpectrogramGraphModifier::setBlackToYellowGradient() {
    //! [8]
    QLinearGradient gr;
    gr.setColorAt(0.f, Qt::black);
    gr.setColorAt(0.33f, Qt::blue);
    gr.setColorAt(0.67f, Qt::red);
    gr.setColorAt(1.f, Qt::yellow);

    m_dataSeries->setBaseGradient(gr);
    m_dataSeries->setColorStyle(Q3DTheme::ColorStyleRangeGradient);
}

void SpectrogramGraphModifier::setGreenToRedGradient() {
    QLinearGradient gr;
    gr.setColorAt(0.f, Qt::darkGreen);
    gr.setColorAt(0.5f, Qt::yellow);
    gr.setColorAt(0.8f, Qt::red);
    gr.setColorAt(1.f, Qt::darkRed);

    m_dataSeries->setBaseGradient(gr);
    m_dataSeries->setColorStyle(Q3DTheme::ColorStyleRangeGradient);
}


void SpectrogramGraphModifier::handleElementSelected(QAbstract3DGraph::ElementType type) {
    resetSelection();
    if (type == QAbstract3DGraph::ElementCustomItem) {
        QCustom3DItem *item = m_graph->selectedCustomItem();
        QString text;
        if (qobject_cast<QCustom3DLabel *>(item) != 0) {
            text.append("Custom label: ");
        } else {
            QStringList split = item->meshFile().split("/");
            text.append(split.last());
            text.append(": ");
        }
        int index = m_graph->selectedCustomItemIndex();
        text.append(QString::number(index));
        m_textField->setText(text);
        m_previouslyAnimatedItem = item;
        m_previousScaling = item->scaling();
        m_selectionAnimation->setTargetObject(item);
        m_selectionAnimation->setStartValue(item->scaling());
        m_selectionAnimation->setEndValue(item->scaling() * 1.5f);
        m_selectionAnimation->start();
    } else if (type == QAbstract3DGraph::ElementSeries) {
        QString text = ui::text("Spectrogram") + " (";
        QSurface3DSeries *series = m_graph->selectedSeries();
        if (series) {
            QPoint point = series->selectedPoint();
            QString posStr;
            posStr.setNum(point.x());
            text.append(posStr);
            text.append(", ");
            posStr.setNum(point.y());
            text.append(posStr);
        }
        text.append(")");
        m_textField->setText(text);
    } else if (type > QAbstract3DGraph::ElementSeries
               && type < QAbstract3DGraph::ElementCustomItem) {
        int index = m_graph->selectedLabelIndex();
        QString text;
        if (type == QAbstract3DGraph::ElementAxisXLabel)
            text.append("Axis X label: ");
        else if (type == QAbstract3DGraph::ElementAxisYLabel)
            text.append("Axis Y label: ");
        else
            text.append("Axis Z label: ");
        text.append(QString::number(index));
        m_textField->setText(text);
    } else {
        m_textField->setText(ui::text("Nothing"));
    }
}

void SpectrogramGraphModifier::resetSelection() {
    m_selectionAnimation->stop();
    if (m_previouslyAnimatedItem)
        m_previouslyAnimatedItem->setScaling(m_previousScaling);
    m_previouslyAnimatedItem = nullptr;
}

void SpectrogramGraphModifier::changePauseState(const Qt::CheckState state) {
}
