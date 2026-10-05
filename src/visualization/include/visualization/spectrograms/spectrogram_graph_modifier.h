// Copyright (C) 2023 The Qt Company Ltd.
// SPDX-License-Identifier: LicenseRef-Qt-Commercial OR BSD-3-Clause

#ifndef SPECTROGRAM_GRAPH_MODIFIER_H
#define SPECTROGRAM_GRAPH_MODIFIER_H

#include <QtDataVisualization/q3dsurface.h>
#include <QtDataVisualization/qsurfacedataproxy.h>
#include <QtDataVisualization/qheightmapsurfacedataproxy.h>
#include <QtDataVisualization/qsurface3dseries.h>
#include <QtDataVisualization/qcustom3ditem.h>
#include <QtDataVisualization/qcustom3dlabel.h>
#include <QtDataVisualization/q3dinputhandler.h>
#include <QtWidgets/qslider.h>
#include <QtWidgets/qlabel.h>
#include <QtCore/qpropertyanimation.h>


class SpectrogramGraphModifier : public QObject {
    Q_OBJECT

public:
    explicit SpectrogramGraphModifier(Q3DSurface *spectrogram, QLabel *label, QObject *parent);


    void toggleModeNone() { m_graph->setSelectionMode(QAbstract3DGraph::SelectionNone); }
    void toggleModeItem() { m_graph->setSelectionMode(QAbstract3DGraph::SelectionItem); }

    void toggleModeSliceRow() {
        m_graph->setSelectionMode(QAbstract3DGraph::SelectionItemAndRow
                                  | QAbstract3DGraph::SelectionSlice
                                  | QAbstract3DGraph::SelectionMultiSeries);
    }

    void toggleModeSliceColumn() {
        m_graph->setSelectionMode(QAbstract3DGraph::SelectionItemAndColumn
                                  | QAbstract3DGraph::SelectionSlice
                                  | QAbstract3DGraph::SelectionMultiSeries);
    }

    void setBlackToYellowGradient();

    void setGreenToRedGradient();

    void setAxisMinSliderX(QSlider *slider) { m_axisMinSliderX = slider; }
    void setAxisMaxSliderX(QSlider *slider) { m_axisMaxSliderX = slider; }
    void setAxisMinSliderZ(QSlider *slider) { m_axisMinSliderZ = slider; }
    void setAxisMaxSliderZ(QSlider *slider) { m_axisMaxSliderZ = slider; }

    void adjustXMin(int min);

    void adjustXMax(int max);

    void adjustZMin(int min);

    void adjustZMax(int max);

    void updateAxisRanges(float xMin, float xMax, float zMin, float zMax);


    void update(const std::vector<std::vector<float> > &spectrogram);

public Q_SLOTS:
    void enable3DModel();

    void changePauseState(Qt::CheckState state);

private:
    void setAxisXRange(float min, float max);

    void setAxisZRange(float min, float max);

    void fillDataProxy();

    void handleElementSelected(QAbstract3DGraph::ElementType type);

    void resetSelection();

private:
    Q3DSurface *m_graph = nullptr;
    QSurfaceDataProxy *m_dataProxy = nullptr;
    QSurface3DSeries *m_dataSeries = nullptr;


    QSlider *m_axisMinSliderX = nullptr;
    QSlider *m_axisMaxSliderX = nullptr;
    QSlider *m_axisMinSliderZ = nullptr;
    QSlider *m_axisMaxSliderZ = nullptr;
    float m_rangeMinX = 0.f;
    float m_rangeMinZ = 0.f;
    float m_stepX = 0.f;
    float m_stepZ = 0.f;

    int sampleCountX = 150;
    int sampleCountZ = 150;
    float sampleMinX = -8.f;
    float sampleMaxX = 8.f;
    float sampleMinZ = -8.f;
    float sampleMaxZ = 8.f;

    QLabel *m_textField = nullptr;
    QPropertyAnimation *m_selectionAnimation = nullptr;
    QCustom3DItem *m_previouslyAnimatedItem = nullptr;
    QVector3D m_previousScaling = {};


    Q3DInputHandler *m_defaultInputHandler = new Q3DInputHandler();
};

#endif // SPECTROGRAM_GRAPH_MODIFIER_H
