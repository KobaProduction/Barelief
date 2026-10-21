#pragma once
#include <QDialog>
#include <QToolButton>
#include "core_app/workspace/processor_directory_model.h"
class QLabel;
class QTreeWidget;
class QVBoxLayout;
class QFormLayout;
class QLineEdit;
class QSpinBox;
class QPushButton;
namespace presentation {
QString processor_caption(const ProcessorEndpoint &endpoint);
class ProcessorManagerDialog final : public QDialog {
public:
    ProcessorManagerDialog(ProcessorDirectoryModel &, QString host, quint16 port, QWidget *parent = nullptr);
    void set_warning(const QString &);
private:
    void refresh();
    void setup_processor_list(QVBoxLayout*);
    void append_registry_status(QWidget*,QFormLayout*);
    void append_processor_row(const ProcessorEndpoint&);
    void setup_direct_connection(QVBoxLayout *layout);
    void setup_registry_connection(QVBoxLayout *layout,const QString& host,quint16 port);
    int populate_processors();
    void update_discovery_status(int available);
    void connect_direct(QLineEdit *host,QSpinBox *port);
    void connect_registry(QLineEdit *host,QSpinBox *port);
    void bind_registry_actions(QPushButton *discover,QPushButton *disconnect,
                               QLineEdit *host,QSpinBox *port);
    ProcessorDirectoryModel &_model;
    QTreeWidget *_list;
    QLabel *_status, *_warning;
};
class ProcessorSelector final : public QToolButton {
public:
    ProcessorSelector(ProcessorDirectoryModel &, QAction *manage, QWidget *parent = nullptr);
};
}
