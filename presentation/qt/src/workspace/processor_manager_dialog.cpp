#include "core_app/workspace/processor_manager_dialog.h"
#include "barelief/ui_text.h"
#include <QTreeWidget>
#include <QHeaderView>
#include <QLabel>
#include <QGroupBox>
#include <QFormLayout>
#include <QVBoxLayout>
#include <QLineEdit>
#include <QSpinBox>
#include <QPushButton>
#include <QMenu>
#include <QHostAddress>

namespace presentation {
QString processor_caption(const ProcessorEndpoint &endpoint) { return endpoint.local ? ui::text("In-app processor") : endpoint.name; }
ProcessorManagerDialog::ProcessorManagerDialog(ProcessorDirectoryModel &model, QString host, quint16 port, QWidget *parent)
    : QDialog(parent), _model(model) {
    setObjectName("engineRegistryDialog"); ui::bind(this,"windowTitle","Manage processors"); resize(600,520);
    auto *layout = new QVBoxLayout(this);
    setup_processor_list(layout);
    setup_direct_connection(layout);
    setup_registry_connection(layout,host,port);
    _warning = new QLabel(this); _warning->setWordWrap(true); layout->addWidget(_warning); set_warning({});
    connect(_list,&QTreeWidget::itemActivated,this,[this](QTreeWidgetItem *item,int) {
        _model.request_selection(item->data(0,Qt::UserRole).toString());
    });
    connect(&_model,&ProcessorDirectoryModel::changed,this,[this] { refresh(); });
    connect(&ui::Language::instance(),&ui::Language::changed,this,[this] { refresh(); });
    refresh();
}

void ProcessorManagerDialog::setup_processor_list(QVBoxLayout* layout) {
    auto *explanation = ui::make<QLabel>("Local processing needs no registry. Discover external processors through an aggregator.",this);
    explanation->setWordWrap(true); layout->addWidget(explanation);
    _list = new QTreeWidget(this); _list->setObjectName("processorManagerList"); _list->setRootIsDecorated(false);
    _list->header()->setSectionResizeMode(QHeaderView::ResizeToContents); layout->addWidget(_list);
}

void ProcessorManagerDialog::setup_direct_connection(QVBoxLayout *layout) {
    auto *direct = ui::make<QGroupBox>("Direct connection",this);
    auto *directForm = new QFormLayout(direct);
    auto *directHost = new QLineEdit("127.0.0.1",direct); directHost->setObjectName("processorHost");
    auto *directPort = new QSpinBox(direct); directPort->setObjectName("processorPort"); directPort->setRange(1,65535); directPort->setValue(8213);
    directForm->addRow(ui::make<QLabel>("Address",direct),directHost); directForm->addRow(ui::make<QLabel>("Port",direct),directPort);
    auto *connectProcessor = ui::make<QPushButton>("Connect processor",direct); connectProcessor->setObjectName("connectProcessor"); directForm->addRow(connectProcessor);
    layout->addWidget(direct);
    connect(connectProcessor,&QPushButton::clicked,this,[this,directHost,directPort] {
        connect_direct(directHost,directPort);
    });
}

void ProcessorManagerDialog::connect_direct(QLineEdit *host_field,QSpinBox *port_field) {
    const QHostAddress address(host_field->text().trimmed());
    if (address.isNull()) { _status->setText(ui::text("Processor address must be an IP address")); return; }
    const auto host=address.toString();
    const auto port=quint16(port_field->value());
    const auto id=host+":"+QString::number(port);
    _model.add_direct(id,host,port);
    _model.request_selection(id);
}

void ProcessorManagerDialog::setup_registry_connection(QVBoxLayout *layout,const QString& host,quint16 port) {
    auto *registry = ui::make<QGroupBox>("Discovery through registry",this); auto *form = new QFormLayout(registry);
    auto *registryHost = new QLineEdit(host,registry); registryHost->setObjectName("registryHost");
    auto *registryPort = new QSpinBox(registry); registryPort->setObjectName("registryPort"); registryPort->setRange(1,65535); registryPort->setValue(port);
    form->addRow(ui::make<QLabel>("Registry address",registry),registryHost); form->addRow(ui::make<QLabel>("Registry port",registry),registryPort);
    auto *discover = ui::make<QPushButton>("Discover engines",registry); discover->setObjectName("discoverEngines"); form->addRow(discover);
    auto *disconnect = ui::make<QPushButton>("Disconnect registry",registry); form->addRow(disconnect);
    append_registry_status(registry,form);layout->addWidget(registry);
    bind_registry_actions(discover,disconnect,registryHost,registryPort);
}
void ProcessorManagerDialog::append_registry_status(QWidget* registry,QFormLayout* form) {
    _status = new QLabel(registry); _status->setObjectName("engineRegistryStatus"); _status->setWordWrap(true); form->addRow(_status);
}
void ProcessorManagerDialog::bind_registry_actions(QPushButton *discover,QPushButton *disconnect,
    QLineEdit *registryHost,QSpinBox *registryPort) {
    connect(discover,&QPushButton::clicked,this,[this,registryHost,registryPort] {
        connect_registry(registryHost,registryPort);
    });
    connect(disconnect,&QPushButton::clicked,this,[this] { _model.disconnect_registry(); });
}
void ProcessorManagerDialog::connect_registry(QLineEdit *host,QSpinBox *port) {
    if (!_model.connect_registry(host->text().trimmed(),quint16(port->value())))
        _status->setText(ui::text("Registry address must be an IP address"));
}
void ProcessorManagerDialog::set_warning(const QString &warning) { _warning->setText(warning); _warning->setVisible(!warning.isEmpty()); }
void ProcessorManagerDialog::refresh() {
    _list->clear(); _list->setHeaderLabels({ui::text("Processor"),ui::text("Endpoint"),ui::text("State")});
    update_discovery_status(populate_processors());
}
void ProcessorManagerDialog::append_processor_row(const ProcessorEndpoint& endpoint) {
        auto *row = new QTreeWidgetItem(_list,{processor_caption(endpoint),endpoint.local ? ui::text("Inside this application") : endpoint.address+":"+QString::number(endpoint.port),
            !endpoint.available ? ui::text("Unavailable") : endpoint.local ? ui::text("Available") : endpoint.allow_mutations ? ui::text("Editing allowed") : ui::text("Read-only engine")});
        row->setData(0,Qt::UserRole,endpoint.id); row->setDisabled(!endpoint.available); row->setToolTip(2,endpoint.error);
        if (endpoint.id == _model.selected_id()) _list->setCurrentItem(row);
}
int ProcessorManagerDialog::populate_processors() {
    int available=0;
    for (const auto &endpoint : _model.processors()) {
        if (endpoint.from_registry && endpoint.available) ++available;
        append_processor_row(endpoint);
    }
    return available;
}
void ProcessorManagerDialog::update_discovery_status(int available) {
    const auto &snapshot = _model.discovery();
    switch (snapshot.state) {
    case DiscoveryState::Disconnected: _status->setText(ui::text("Registry disconnected. Local processing remains available.")); break;
    case DiscoveryState::Connecting: _status->setText(ui::text("Connecting to registry…")); break;
    case DiscoveryState::Connected: _status->setText(available ? ui::text("Available external processors: %1").arg(available) : ui::text("Registry connected; no external processors with a control endpoint.")); break;
    case DiscoveryState::Unavailable: _status->setText(ui::text("Registry unavailable: %1").arg(ui::text(snapshot.error.toUtf8().constData()))); break;
    }
}
namespace {
void populate_selector_menu(QMenu *menu,ProcessorDirectoryModel& model,QAction *manage) {
    menu->clear();
    for (const auto &endpoint : model.processors()) {
        auto *action = menu->addAction(processor_caption(endpoint));
        action->setEnabled(endpoint.available);
        action->setCheckable(true);
        action->setChecked(endpoint.id == model.selected_id());
        action->setToolTip(endpoint.error);
        QObject::connect(action,&QAction::triggered,menu,
            [&model,id=endpoint.id] { model.request_selection(id); });
    }
    menu->addSeparator();
    menu->addAction(manage);
}
void refresh_selector_caption(QToolButton *button,ProcessorDirectoryModel& model) {
    if (const auto *endpoint = model.selected()) {
        button->setText(processor_caption(*endpoint));
        button->setToolTip(button->text());
    }
}
}
ProcessorSelector::ProcessorSelector(ProcessorDirectoryModel &model, QAction *manage, QWidget *parent) : QToolButton(parent) {
    ui::compact_control(this);
    setObjectName("engineSelectorButton"); setMaximumWidth(260);
    auto *menu = new QMenu(this); ui::button_menu(this,menu);
    connect(menu,&QMenu::aboutToShow,this,
        [menu,&model,manage] { populate_selector_menu(menu,model,manage); });
    const auto refresh = [this,&model] { refresh_selector_caption(this,model); };
    connect(&model,&ProcessorDirectoryModel::changed,this,refresh);
    connect(&ui::Language::instance(),&ui::Language::changed,this,refresh); refresh();
}
}
