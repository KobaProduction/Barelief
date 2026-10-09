#pragma once
#include <QDialog>
#include <QToolButton>
#include "core_app/workspace/processor_directory_model.h"
class QLabel;
class QTreeWidget;
namespace presentation {
QString processor_caption(const ProcessorEndpoint &endpoint);
class ProcessorManagerDialog final : public QDialog {
public:
    ProcessorManagerDialog(ProcessorDirectoryModel &, QString host, quint16 port, QWidget *parent = nullptr);
    void set_warning(const QString &);
private:
    void refresh();
    ProcessorDirectoryModel &_model;
    QTreeWidget *_list;
    QLabel *_status, *_warning;
};
class ProcessorSelector final : public QToolButton {
public:
    ProcessorSelector(ProcessorDirectoryModel &, QAction *manage, QWidget *parent = nullptr);
};
}
