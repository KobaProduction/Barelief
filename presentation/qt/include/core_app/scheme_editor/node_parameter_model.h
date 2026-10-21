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
        QStringList label_arguments;
    };
    struct Field {
        QString key, label, help, units;
        QString bounds_error, availability_error;
        QStringList caption_arguments;
        QString special_value_text;
        Section section = Section::Processing;
        Editor editor = Editor::Text;
        int order = 0, minimum = 0, maximum = 0;
        std::optional<double> numeric_minimum, numeric_maximum;
        std::optional<int> literal_type; // Fixed representation for sizes and operation parameters.
        bool pinnable = true, primary = false, hide_inline = false, hide_inspector = false, translate_value = false;
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
    void setAdmissionLimits(std::optional<domain::ProcessingAdmissionLimits>);
    const std::vector<Field> &fields() const {
        return _fields;
    }
    const Field *field(const QString &) const;
    bool edit(const QString &, const Value &);
    // Routing points have no execution mode.
    bool supportsBypass() const;
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
    std::optional<domain::ProcessingAdmissionLimits> _admission_limits;
    std::optional<std::size_t> profilePortLimit() const;
    void requireProfile();
    bool commit(domain::ConfiguredProcessingNode);
    bool requiresStructure(const domain::ConfiguredProcessingNode&) const;
    bool remapOscillatorDrafts(const domain::SignalGeneratorProcessingParameters&,const domain::SignalGeneratorProcessingParameters&);
    void clearOscillatorDrafts(QMap<QString,EditState>&) const;
    bool copyMappedOscillatorDrafts(const domain::SignalGeneratorProcessingParameters&,
        const domain::SignalGeneratorProcessingParameters&,QMap<QString,EditState>&) const;
    void copyOscillatorDrafts(std::size_t,std::size_t,QMap<QString,EditState>&) const;
    bool finishEdit(const QString&,bool);
    Section _field_section=Section::Route;
    Field makeField(QString,QString,Editor);
    template<class Read,class Write> void literal(QString,QString,Read,Write,std::optional<int> fixed_type=std::nullopt);
    template<class Read,class Write> void choice(QString,QString,QStringList,Read,Write);
    template<class Read,class Write> void boolean(QString,QString,Read,Write);
    template<class Read,class Write> void text(QString,QString,Read,Write);
    template<class Read,class Write> void integer(QString,QString,int,int,Read,Write);
    void append_routing_fields();
    void appendFactorCount();
    void appendSelectorFields();
    void appendRoutingOutputCount();
    void appendSwitchFields();
    void append_generators_fields();
    void appendSampleClockFields();
    void appendOscillatorBankActions();
    void appendOscillatorCount();
    void appendOscillatorActions(std::size_t);
    void appendOscillatorFields(std::size_t);
    void appendOscillatorProperty(std::size_t,std::size_t);
    bool applyOscillatorBank(int);
    bool addOscillator();
    bool removeOscillator(std::size_t);
    void append_mixing_fields();
    void appendMixerTiming();
    void appendMixerWeight(domain::PortId);
    QString mixerShare(domain::PortId) const;
    void append_music_fields();
    void append_coefficient_summary();
    void appendReadout(QString,QString,std::function<QString()>,bool primary=false);
    QString musicRowLayout() const;
    QString noteGridDescription() const;
    QString frequencyProfileStatus() const;
    double parameterNumber(domain::NodeKind,std::string_view) const;
    void append_accumulation_fields();
    void append_registered_fields();
    domain::ProcessingParameterValue parameterValue(const domain::ParameterDescriptor&) const;
    static void putParameter(domain::ConfiguredProcessingNode&,std::string_view,domain::ProcessingParameterValue);
    bool parameterVisible(const domain::ParameterDescriptor&) const;
    void appendParameterField(const domain::ParameterDescriptor&);
    void appendInvalidParameter(const QString&,const QString&);
    void configureParameterAvailability(const domain::ParameterDescriptor&);
    bool writeParameterChoice(const domain::ParameterDescriptor&,const QStringList&,int);
    void appendParameterChoice(const domain::ParameterDescriptor&,const QString&,const QStringList&,const QStringList&);
    void appendBooleanParameter(const domain::ParameterDescriptor&,const QString&,const QString&);
    void appendTextParameter(const domain::ParameterDescriptor&,const QString&,const QString&);
    void appendNonNumericParameter(const domain::ParameterDescriptor&,const QString&,const QString&);
    void appendParameterValue(const domain::ParameterDescriptor&,const QString&);
    void append_contracts_fields();
    void appendContractCaption(const domain::ProcessingPort&,domain::PortDirection);
    void appendContractDtype(domain::PortDirection,domain::PortId);
    void appendContractShape(domain::PortDirection,domain::PortId);
    void appendContractExtent(domain::PortDirection,domain::PortId,std::size_t);
    void appendContractRepresentation(domain::PortDirection,domain::PortId);
    void appendContractPort(const domain::ProcessingPort&,domain::PortDirection);
    void append_conversion_fields();
    void appendConverterPolicy();
    void appendConverterScaling();
    void appendConverterExponents();
    void appendConverterReplacements();
    bool resetSettings();
    bool resetSelectorSettings(domain::ConfiguredProcessingNode&) const;
    void append_devices_fields();
    QJsonValue adapterValue(const QString&,bool) const;
    bool writeAdapterValue(const QString&,bool,const QJsonValue&);
    void appendAdapterText(const char*,const char*,const char*,bool endpoint=false);
    void appendAdapterNumber(const char*,const char*,const char*,int,int,bool endpoint=false);
    void appendNetworkAdapterFields();
    void appendRemoteEndpointFields();
    QString captureSavedFormat() const;
    Field captureDeviceField(QString&);
    void appendCaptureChoice();
    void appendCaptureFields();
    void buildFields();
};
