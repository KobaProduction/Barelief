#pragma once
#include <QObject>
#include <QStringList>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <domain/processing_contracts.h>
#include <functional>
#include <map>
#include <optional>
#include <variant>

// Presentation state and edit requests. No dependency on a widget or QtNodes.
class NodeParameterModel final : public QObject {
    Q_OBJECT
  public:
    enum class Section { Route, Processing, Contract, Additional };
    enum class Editor { Literal, Choice, Boolean, Text, Integer, Caption, Action };
    using Value = std::variant<signal_contracts::NumericLiteral, int, bool, QString>;
    struct Option {
        QString label;
        Value value;
        bool translate = true;
    };
    struct Field {
        QString key, label, help, units;
        Section section = Section::Processing;
        Editor editor = Editor::Text;
        int order = 0, minimum = 0, maximum = 0;
        std::optional<double> numeric_minimum, numeric_maximum;
        bool pinnable = true, primary = false, hide_inline = false, translate_value = false;
        std::vector<Option> options;
        std::function<Value()> read;
        std::function<bool(const Value &)> write;
        std::function<bool()> available;
    };
    struct EditState {
        QString input, error;
        int literal_type = -1;
        QStringList error_arguments;
        bool dirty = false;
    };
    explicit NodeParameterModel(QObject *parent = nullptr);
    void setNode(const domain::ConfiguredProcessingNode &, bool rebuild);
    void setAdapter(QJsonObject);
    void setPresentation(QJsonObject);
    void setEditHandlers(std::function<bool(const domain::ConfiguredProcessingNode &)>,
                         std::function<bool(const QJsonObject &)>, std::function<bool(const QString &, bool)>);
    void setCaptureDeviceProvider(std::function<QJsonArray(int, QString *)>);
    const std::vector<Field> &fields() const {
        return _fields;
    }
    const Field *field(const QString &) const;
    bool edit(const QString &, const Value &);
    // Node execution mode is always available, outside the pinnable fields.
    bool bypassed() const { return !_node.enabled; }
    bool setBypassed(bool);
    bool editLiteral(const QString &, const QString &, int type);
    void setDraftInput(const QString &, const QString &, int literal_type = -1);
    EditState editState(const QString &) const;
    bool onBlock(const QString &) const;
    bool setOnBlock(const QString &, bool);
    bool expanded(Section) const;
    void setExpanded(Section, bool);
    static const char *sectionTitle(Section);
  signals:
    void valuesChanged();
    void structureChanged();
    void presentationChanged();
    void editStateChanged(const QString &);

  private:
    domain::ConfiguredProcessingNode _node;
    QJsonObject _adapter, _presentation;
    std::vector<Field> _fields;
    QMap<QString, EditState> _edits;
    std::map<std::pair<domain::NodeId, int>, bool> _expanded;
    std::function<bool(const domain::ConfiguredProcessingNode &)> _edit_node;
    std::function<bool(const QJsonObject &)> _edit_adapter;
    std::function<bool(const QString &, bool)> _edit_presentation;
    std::function<QJsonArray(int, QString *)> _capture_devices;
    bool commit(domain::ConfiguredProcessingNode);
    void buildFields();
};
