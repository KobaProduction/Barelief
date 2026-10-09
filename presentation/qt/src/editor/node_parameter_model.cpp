#include "core_app/scheme_editor/node_parameter_model.h"
#include "core_app/scheme_editor/numeric_node_operations.h"
#include "core_app/scheme_editor/typed_parameter_helper.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <cmath>
NodeParameterModel::NodeParameterModel(QObject *parent) : QObject(parent) {}
void NodeParameterModel::setEditHandlers(std::function<bool(const domain::ConfiguredProcessingNode &)> node,
                                         std::function<bool(const QJsonObject &)> adapter,
                                         std::function<bool(const QString &, bool)> presentation) {
    _edit_node = std::move(node);
    _edit_adapter = std::move(adapter);
    _edit_presentation = std::move(presentation);
}
void NodeParameterModel::setNode(const domain::ConfiguredProcessingNode &n, bool rebuild) {
    const bool first = _fields.empty();
    if (_node.id != n.id)
        _edits.clear();
    _node = n;
    buildFields();
    if (rebuild || first)
        emit structureChanged();
    else
        emit valuesChanged();
}
void NodeParameterModel::setAdapter(QJsonObject a) {
    _adapter = std::move(a);
    if (!_node.kind.empty())
        buildFields();
    emit valuesChanged();
}
void NodeParameterModel::setPresentation(QJsonObject p) {
    _presentation = std::move(p);
    emit presentationChanged();
}
void NodeParameterModel::setCaptureDeviceProvider(std::function<QJsonArray(int, QString *)> p) {
    _capture_devices = std::move(p);
    if (!_node.kind.empty()) {
        buildFields();
        emit structureChanged();
    }
}
const NodeParameterModel::Field *NodeParameterModel::field(const QString &key) const {
    for (const auto &f : _fields)
        if (f.key == key)
            return &f;
    return nullptr;
}
void NodeParameterModel::setDraftInput(const QString &key, const QString &input, int type) {
    auto &state = _edits[key];
    state.input = input;
    state.literal_type = type;
    state.dirty = true;
    emit editStateChanged(key);
}
NodeParameterModel::EditState NodeParameterModel::editState(const QString &key) const {
    return _edits.value(key);
}
bool NodeParameterModel::commit(domain::ConfiguredProcessingNode n) {
    return _edit_node && _edit_node(n);
}
bool NodeParameterModel::setBypassed(bool bypassed) {
    if(bypassed==!_node.enabled)return true;
    auto n=_node;n.enabled=!bypassed;
    const bool accepted=commit(std::move(n));
    // Refused requests must restore every bound control as well as accepted
    // edits/undo updating the shared model through the ordinary command path.
    emit valuesChanged();
    return accepted;
}
bool NodeParameterModel::edit(const QString &key, const Value &value) {
    const auto *f = field(key);
    if (!f || !f->write || (f->available && !f->available()))
        return false;
    if (f->editor == Editor::Text && std::holds_alternative<QString>(value)) {
        _edits[key].input = std::get<QString>(value);
        _edits[key].dirty = true;
    }
    if (f->read && f->read() == value) {
        _edits.remove(key);
        emit editStateChanged(key);
        emit valuesChanged();
        return true;
    }
    const auto write = f->write;
    bool accepted = false;
    try {
        bool inRange = true;
        if (f->editor == Editor::Integer) {
            const auto n = std::get<int>(value);
            inRange = n >= f->minimum && n <= f->maximum;
        }
        if (f->editor == Editor::Choice)
            inRange = std::any_of(f->options.begin(), f->options.end(),
                                  [&](const auto &option) { return option.value == value; });
        if (inRange)
            accepted = write(value);
    } catch (const std::invalid_argument &) {
    } catch (const std::bad_variant_access &) {
    }
    if (accepted)
        _edits.remove(key);
    else
        _edits[key].error = "Unsupported node settings";
    emit editStateChanged(key);
    emit valuesChanged();
    return accepted;
}
bool NodeParameterModel::editLiteral(const QString &key, const QString &text, int type) {
    const auto *f = field(key);
    if (!f || f->editor != Editor::Literal)
        return false;
    _edits[key] = EditState{text, {}, type, {}, true};
    signal_contracts::NumericLiteral v;
    switch (type) {
    case 0:
        v = std::int64_t{};
        break;
    case 1:
        v = std::uint64_t{};
        break;
    case 2:
        v = float{};
        break;
    case 3:
        v = double{};
        break;
    default:
        return false;
    }
    bool valid = numeric_editor::parse_literal(text, v, v);
    if (valid && (f->numeric_minimum || f->numeric_maximum))
        valid = std::visit(
            [&](auto number) {
                const long double n = number;
                return (!f->numeric_minimum || n >= *f->numeric_minimum) &&
                       (!f->numeric_maximum || n <= *f->numeric_maximum);
            },
            v);
    if (!valid) {
        const QStringList types{"int64", "uint64", "float32", "float64"};
        auto low = std::visit(
            [](auto number) {
                using T = decltype(number);
                return numeric_editor::literal_text(signal_contracts::NumericLiteral{std::numeric_limits<T>::lowest()});
            },
            v);
        auto high = std::visit(
            [](auto number) {
                using T = decltype(number);
                return numeric_editor::literal_text(signal_contracts::NumericLiteral{std::numeric_limits<T>::max()});
            },
            v);
        if (f->numeric_minimum)
            low = QString::number(*f->numeric_minimum, 'g', 17);
        if (f->numeric_maximum)
            high = QString::number(*f->numeric_maximum, 'g', 17);
        _edits[key].error = "Enter a valid %1 value between %2 and %3.";
        _edits[key].error_arguments = {types[type], low, high};
        emit editStateChanged(key);
        return false;
    }
    return edit(key, Value{v});
}
bool NodeParameterModel::onBlock(const QString &key) const {
    const auto *f = field(key);
    return f && (f->primary || _presentation.value("inline_parameters").toArray().contains(key));
}
bool NodeParameterModel::setOnBlock(const QString &key, bool shown) {
    const auto* f=field(key);
    return f && f->pinnable && _edit_presentation && _edit_presentation(key, shown);
}
bool NodeParameterModel::expanded(Section s) const {
    const auto i = _expanded.find({_node.id, int(s)});
    return i != _expanded.end() ? i->second : int(s) < 2;
}
void NodeParameterModel::setExpanded(Section s, bool v) {
    _expanded[{_node.id, int(s)}] = v;
}
const char *NodeParameterModel::sectionTitle(Section s) {
    switch (s) {
    case Section::Route:
        return "Source / route";
    case Section::Processing:
        return "Processing";
    case Section::Contract:
        return "Data contract";
    default:
        return "Additional settings";
    }
}
void NodeParameterModel::buildFields() {
    using namespace signal_contracts;
    using namespace domain;
    _fields.clear();
    const auto _kind = QString::fromStdString(_node.kind);
    Section section = Section::Route;
    auto makeField = [&](QString label, QString key, Editor editor) {
        Field f;
        f.label = std::move(label);
        f.key = std::move(key);
        f.editor = editor;
        f.section = section;
        f.order = int(_fields.size());
        f.primary = (f.key == "numericSelected" && (_kind == "InputSelector" || _kind == "Switch")) ||
                    (f.key == "numericGate" && _kind == "Gate") || (f.key == "numericOutputs" && _kind == "Splitter");
        f.hide_inline = _kind == "InputSelector" && (f.key == "numericSelected" || f.key == "numericInputs");
        return f;
    };
    auto literal = [&](QString label, QString key, auto read, auto write) {
        auto f = makeField(label, key, Editor::Literal);
        f.read = [this, read]() -> Value { return read(_node); };
        f.write = [this, write](const Value &v) {
            auto n = _node;
            write(n, std::get<NumericLiteral>(v));
            return commit(std::move(n));
        };
        _fields.push_back(std::move(f));
    };
    auto choice = [&](QString label, QString key, QStringList names, auto read, auto write) {
        auto f = makeField(label, key, Editor::Choice);
        const bool selector = _kind == "InputSelector" && key == "numericSelected";
        if (selector) {
            for (const auto &p : _node.inputs)
                f.options.push_back({"Input %1", int(p.id), true});
        } else
            for (int i = 0; i < names.size(); ++i)
                f.options.push_back({names[i], i, true});
        f.read = [this, read]() -> Value { return int(read(_node)); };
        f.write = [this, write](const Value &v) {
            auto n = _node;
            write(n, std::get<int>(v));
            return commit(std::move(n));
        };
        _fields.push_back(std::move(f));
    };
    auto boolean = [&](QString label, QString key, auto read, auto write) {
        auto f = makeField(label, key, Editor::Boolean);
        f.read = [this, read]() -> Value { return bool(read(_node)); };
        f.write = [this, write](const Value &v) {
            auto n = _node;
            write(n, std::get<bool>(v));
            return commit(std::move(n));
        };
        _fields.push_back(std::move(f));
    };
    auto text = [&](QString label, QString key, auto read, auto write) {
        auto f = makeField(label, key, Editor::Text);
        f.read = [this, read]() -> Value { return read(_node); };
        f.write = [this, write](const Value &v) {
            auto n = _node;
            write(n, std::get<QString>(v));
            return commit(std::move(n));
        };
        _fields.push_back(std::move(f));
    };
    auto integer = [&](QString label, QString key, int minimum, int maximum, auto read, auto write) {
        auto f = makeField(label, key, Editor::Integer);
        f.minimum = minimum;
        f.maximum = maximum;
        f.read = [this, read]() -> Value { return int(read(_node)); };
        f.write = [this, write](const Value &v) {
            auto n = _node;
            write(n, std::get<int>(v));
            return commit(std::move(n));
        };
        _fields.push_back(std::move(f));
    };
    if (_kind == "InputSelector")
        integer(
            "Input count", "numericInputs", 1, int(numeric_editor::limits().max_ports_per_node),
            [](const auto &n) { return int(n.inputs.size()); },
            [](auto &n, int v) {
                if (!numeric_editor::set_input_count(n, std::uint16_t(v)))
                    throw std::invalid_argument("count");
            });
    if (std::holds_alternative<RouteProcessingParameters>(_node.parameters)) {
        if (_kind == "Splitter" || _kind == "Switch")
            integer(
                "Output count", "numericOutputs", 2, 8,
                [](const auto &n) { return int(std::get<RouteProcessingParameters>(n.parameters).outputs); },
                [](auto &n, int v) {
                    if (!numeric_editor::set_output_count(n, std::uint16_t(v)))
                        throw std::invalid_argument("count");
                });
        if (_kind == "Switch")
            integer(
                "Active output (zero based)", "numericSelected", 0,
                std::get<RouteProcessingParameters>(_node.parameters).outputs - 1,
                [](const auto &n) { return int(std::get<RouteProcessingParameters>(n.parameters).selected); },
                [](auto &n, int v) { std::get<RouteProcessingParameters>(n.parameters).selected = std::uint16_t(v); });
        if (_kind == "Gate")
            choice(
                "Gate", "numericGate", {"Closed", "Open"},
                [](const auto &n) { return std::get<RouteProcessingParameters>(n.parameters).open ? 1 : 0; },
                [](auto &n, int v) { std::get<RouteProcessingParameters>(n.parameters).open = v == 1; });
    }
    section = Section::Processing;
    if (std::holds_alternative<HistoryProcessingParameters>(_node.parameters))
        literal(
            "History capacity, frames", "numericCapacity",
            [](const auto &n) { return NumericLiteral{std::get<HistoryProcessingParameters>(n.parameters).capacity}; },
            [](auto &n, const auto &v) {
                if (const auto *value = std::get_if<std::uint64_t>(&v)) {
                    auto data = n.outputs[0].contract.view();
                    std::get<NumericContract>(data).shape.extents[0].maximum = *value;
                    if (numeric_editor::set_contract(n, PortDirection::Output, 0, data))
                        std::get<HistoryProcessingParameters>(n.parameters).capacity = *value;
                    else
                        throw std::invalid_argument("capacity");
                } else
                    throw std::invalid_argument("capacity type");
            });
    if (std::holds_alternative<ReduceProcessingParameters>(_node.parameters))
        choice(
            "Operation", "numericReduction", {"Maximum absolute", "Mean", "RMS"},
            [](const auto &n) { return int(std::get<ReduceProcessingParameters>(n.parameters).operation); },
            [](auto &n, int v) { std::get<ReduceProcessingParameters>(n.parameters).operation = NumericReduction(v); });
    if (std::holds_alternative<EndpointProcessingParameters>(_node.parameters) ||
        std::holds_alternative<TransformProcessingParameters>(_node.parameters)) {
        const auto kind = domain::node_kind(_node.kind);
        if (kind)
            for (const auto &descriptor : domain::parameter_descriptors(*kind)) {
                if (descriptor.classification != ParameterClassification::Processing || descriptor.key == "enabled" ||
                    descriptor.value_kind == ParameterValueKind::FrameType)
                    continue;
                const auto key = std::string(descriptor.key);
                const auto field = QString::fromStdString(key);
                const QMap<QString, const char *> titles{{"source", "Capture source"},
                                                         {"merge", "Merge mode"},
                                                         {"gain", "Gain"},
                                                         {"bands", "Mel bands"},{"channel_index","Channel index"},{"index","Element index"},{"column","Column index"},
                                                         {"size", "Window size, samples"},
                                                         {"function", "Window function"},
                                                         {"tones", "Tones"},
                                                         {"sample_rate", "Sample rate, Hz"},
                                                         {"output_rate", "Output sample rate, Hz"},
                                                         {"minimum_hz", "Minimum frequency, Hz"},
                                                         {"maximum_hz", "Maximum frequency, Hz"},
                                                         {"amplitude", "Amplitude"},
                                                         {"period_seconds", "Period, seconds"},
                                                         {"sweep_depth", "Sweep depth"},
                                                         {"destination", "Destination IP address"},
                                                         {"interface", "Interface IP address"},
                                                         {"port", "Port"},
                                                         {"cadence_ms", "Send interval, ms"}};
                const auto *title = titles.value(field, key.c_str());
                auto get = [key, descriptor](const ConfiguredProcessingNode &n) -> ProcessingParameterValue {
                    const auto *values = std::get_if<EndpointProcessingParameters>(&n.parameters)
                                             ? &std::get<EndpointProcessingParameters>(n.parameters).values
                                             : &std::get<TransformProcessingParameters>(n.parameters).values;
                    for (const auto &p : *values)
                        if (p.key == key)
                            return p.value;
                    ResolvedProcessingNode resolved;
                    ProcessingParameterView value;
                    if (resolve_processing_node(n, numeric_editor::limits(), resolved) &&
                        resolve_processing_parameter(resolved, key, value) == Status::ok) {
                        if (const auto *v = std::get_if<NumericLiteral>(&value))
                            return *v;
                        if (const auto *v = std::get_if<bool>(&value))
                            return *v;
                        return std::string(std::get<std::string_view>(value));
                    }
                    // An invalid draft remains editable with the same registered defaults.
                    if (descriptor.value_kind == ParameterValueKind::Integer)
                        return NumericLiteral{std::int64_t(descriptor.default_number)};
                    if (descriptor.value_kind == ParameterValueKind::Number)
                        return NumericLiteral{descriptor.default_number};
                    if (descriptor.value_kind == ParameterValueKind::Boolean)
                        return descriptor.default_number != 0;
                    return std::string(descriptor.default_text);
                };
                auto put = [key](ConfiguredProcessingNode &n, ProcessingParameterValue value) {
                    auto *values = std::get_if<EndpointProcessingParameters>(&n.parameters)
                                       ? &std::get<EndpointProcessingParameters>(n.parameters).values
                                       : &std::get<TransformProcessingParameters>(n.parameters).values;
                    for (auto &p : *values)
                        if (p.key == key) {
                            p.value = std::move(value);
                            return;
                        }
                    values->push_back({key, std::move(value)});
                };
                if (_kind == "InputSelector" && key == "selected_input")
                    choice(
                        "Selected input", "numericSelected", {"Input 0", "Input 1"},
                        [get](const auto &n) {
                            return std::visit([](auto value) { return int(value); }, std::get<NumericLiteral>(get(n)));
                        },
                        [put](auto &n, int value) { put(n, NumericLiteral{std::int64_t(value)}); });
                else if (_kind == "AudioSource" && key == "source")
                    choice(
                        "Capture source", "source", {"System audio (loopback)", "Microphone"},
                        [](const auto &n) { return numeric_editor::capture_source_kind(n); },
                        [put](auto &n, int value) { put(n, NumericLiteral{std::int64_t(value)}); });
                else if (key=="sample_rate" || key=="output_rate")
                    literal(title,field,[](const auto& n){
                        const auto contract=n.outputs[0].contract.view();return NumericLiteral{std::uint64_t(std::get<NumericContract>(contract).clock.sample_rate)};
                    },[](auto& n,NumericLiteral v){
                        const auto minimum=n.kind=="AudioSource"?1000u:8000u;
                        const auto rate=std::visit([minimum](auto number){return number>=minimum && number<=192000 && std::floor(double(number))==double(number)?std::uint32_t(number):0;},v);
                        if(!numeric_editor::set_sample_rate(n,rate))throw std::invalid_argument("sample rate");
                    });
                else if (descriptor.value_kind == ParameterValueKind::Integer ||
                         descriptor.value_kind == ParameterValueKind::Number)
                    literal(
                        title, field, [get](const auto &n) { return std::get<NumericLiteral>(get(n)); },
                        [put](auto &n, NumericLiteral v) { put(n, v); });
                else if (descriptor.value_kind == ParameterValueKind::Boolean)
                    boolean(
                        title, field, [get](const auto &n) { return std::get<bool>(get(n)); },
                        [put](auto &n, bool v) { put(n, v); });
                else
                    text(
                        title, field,
                        [get](const auto &n) { return QString::fromStdString(std::get<std::string>(get(n))); },
                        [put](auto &n, const QString &v) { put(n, v.toStdString()); });
                if (!_fields.empty()) {
                    auto &f = _fields.back();
                    const QMap<QString, QString> units{{"sample_rate", "Hz"}, {"output_rate", "Hz"}, {"minimum_hz", "Hz"},
                                                       {"maximum_hz", "Hz"},  {"period_seconds", "s"},
                                                       {"cadence_ms", "ms"},  {"size", "samples"}};
                    f.units = units.value(field);
                    if (key == "source" || key == "selected_input")
                        f.section = Section::Route;
                    if (descriptor.value_kind == ParameterValueKind::Integer ||
                        descriptor.value_kind == ParameterValueKind::Number) {
                        f.numeric_minimum = descriptor.minimum;
                        f.numeric_maximum = descriptor.maximum;
                    }
                }
            }
    }

    section = Section::Contract;
    for (auto direction : {PortDirection::Input, PortDirection::Output}) {
        const auto &ports = direction == PortDirection::Input ? _node.inputs : _node.outputs;
        for (const auto &port : ports) {
            auto captionField = makeField(
                "", QString(direction == PortDirection::Input ? "input%1Caption" : "output%1Caption").arg(port.id),
                Editor::Caption);
            captionField.pinnable = false;
            captionField.read = [this, direction, id = port.id, _kind]() -> Value {
                const auto &ps = direction == PortDirection::Input ? _node.inputs : _node.outputs;
                for (const auto &p : ps)
                    if (p.id == id)
                        return numeric_editor::port_caption(p,!numeric_editor::is_graph_view(_kind));
                return QString{};
            };
            _fields.push_back(std::move(captionField));
            auto data = port.contract.view();
            if (numeric_editor::is_graph_view(_kind) || !std::holds_alternative<NumericContract>(data))
                continue;
            const auto id = port.id;
            QStringList names;
            for (const auto &d : dtype_descriptors())
                names.append(numeric_editor::dtype_caption(d.type));
            const auto key = QString(direction == PortDirection::Input ? "input%1Dtype" : "output%1Dtype").arg(id);
            choice(
                direction == PortDirection::Input ? "Input dtype" : "Output dtype", key, names,
                [direction, id](const auto &n) {
                    const auto &ps = direction == PortDirection::Input ? n.inputs : n.outputs;
                    for (const auto &p : ps)
                        if (p.id == id)
                            return int(std::get<NumericContract>(p.contract.view()).dtype);
                    return -1;
                },
                [direction, id](auto &n, int value) {
                    if (!numeric_editor::set_dtype(n, direction, id, DType(value)))
                        throw std::invalid_argument("dtype");
                });
            const auto prefix = QString(direction == PortDirection::Input ? "input%1" : "output%1").arg(id);
            auto read = [direction, id](const ConfiguredProcessingNode &node) {
                const auto &ps = direction == PortDirection::Input ? node.inputs : node.outputs;
                for (const auto &p : ps)
                    if (p.id == id)
                        return std::get<NumericContract>(p.contract.view());
                return NumericContract{};
            };
            auto modify = [this, direction, id, read](ConfiguredProcessingNode &node,
                                                      const std::function<void(NumericContract &)> &change) {
                DataContract data = read(node);
                change(std::get<NumericContract>(data));
                if (!numeric_editor::set_contract(node, direction, id, data)) {
                    throw std::invalid_argument("Unsupported node settings");
                }
            };
            choice(
                "Shape", prefix + "Rank", {"Scalar", "Vector", "Matrix"},
                [read](const auto &n) { return int(read(n).shape.rank); },
                [modify](auto &n, int v) {
                    modify(n, [v](auto &c) {
                        c.shape.rank = Rank(v);
                        if (c.shape.rank == Rank::Scalar) {
                            c.shape = {};
                            c.coordinates = {};
                            c.channel_order = c.channel_order.first(1);
                        } else {
                            if (c.shape.axes[0] == Axis::None)
                                c.shape.axes[0] = Axis::Declared;
                            if (c.shape.rank == Rank::Matrix) {
                                if (c.shape.axes[1] == Axis::None)
                                    c.shape.axes[1] = Axis::Declared;
                            } else {
                                c.shape.axes[1] = Axis::None;
                                c.shape.extents[1] = {1, 1};
                                c.coordinates[1] = {};
                            }
                        }
                    });
                });
            for (std::size_t axis = 0; axis < 2; ++axis)
                literal(
                    axis == 0 ? "First extent" : "Second extent", prefix + QString("Extent%1").arg(axis),
                    [read, axis](const auto &n) { return NumericLiteral{read(n).shape.extents[axis].maximum}; },
                    [modify, axis](auto &n, const auto &v) {
                        if (const auto *extent = std::get_if<std::uint64_t>(&v))
                            modify(n, [axis, extent](auto &c) {
                                c.shape.extents[axis] = {*extent,
                                                         *extent};
                            });
                        else
                            throw std::invalid_argument("extent type");
                    });
            for (std::size_t axis = 0; axis < 2; ++axis) {
                const auto extentKey = prefix + QString("Extent%1").arg(axis);
                for (auto &f : _fields)
                    if (f.key == extentKey)
                        f.available = [this, read, axis] { return std::size_t(read(_node).shape.rank) > axis; };
            }
            choice(
                "Unit", prefix + "Unit",
                {"Unspecified", "Normalized amplitude", "Raw amplitude", "Frames", "Hz", "s", "Values"},
                [read](const auto &n) { return int(read(n).unit); },
                [modify](auto &n, int v) { modify(n, [v](auto &c) { c.unit = NumericUnit(v); }); });
            choice(
                "Layout", prefix + "Layout", {"Planar", "Interleaved"},
                [read](const auto &n) { return int(read(n).representation.layout); },
                [modify](auto &n, int v) { modify(n, [v](auto &c) { c.representation.layout = ChannelLayout(v); }); });
            choice(
                "Encoding", prefix + "Encoding",
                {"Native", "Little endian", "Big endian", "Packed PCM24 LE", "Packed PCM24 BE"},
                [read](const auto &n) { return int(read(n).representation.encoding); },
                [modify](auto &n, int v) {
                    modify(n, [v](auto &c) {
                        c.representation.encoding = StorageEncoding(v);
                        if (v >= 3) {
                            c.dtype = DType::Int32;
                            c.representation.valid_bits = 24;
                            c.representation.placement = BitPlacement::LeastSignificant;
                        } else
                            c.representation.valid_bits = 0;
                    });
                });
            choice(
                "Normalization", prefix + "Normalization", {"None", "Signed unit", "Unsigned unit", "Float unit"},
                [read](const auto &n) { return int(read(n).representation.normalization); },
                [modify](auto &n, int v) {
                    modify(n, [v](auto &c) { c.representation.normalization = Normalization(v); });
                });
            integer(
                "Channels", prefix + "Channels", 1, int(numeric_editor::limits().frame.max_planes),
                [read](const auto &n) { return int(read(n).channel_order.size()); },
                [read, modify](auto &n, int count) {
                    const auto c = read(n);
                    std::vector<std::uint32_t> ids(c.channel_order.begin(), c.channel_order.end());
                    ids.resize(std::size_t(count));
                    for (std::size_t i = c.channel_order.size(); i < ids.size(); ++i) {
                        std::uint32_t candidate = 0;
                        while (std::find(ids.begin(), ids.begin() + std::ptrdiff_t(i), candidate) !=
                               ids.begin() + std::ptrdiff_t(i))
                            ++candidate;
                        ids[i] = candidate;
                    }
                    modify(n, [&ids](auto &value) { value.channel_order = ids; });
                });
        }
    }

    section = Section::Additional;
    if (std::holds_alternative<ConverterProcessingParameters>(_node.parameters)) {
        section = Section::Processing;
        auto policy = [](auto &n) -> auto & { return std::get<ConverterProcessingParameters>(n.parameters).policy; };
        choice(
            "Rounding", "numericRounding", {"Toward zero", "Nearest ties to even"},
            [policy](const auto &n) { return int(policy(n).rounding); },
            [policy](auto &n, int v) { policy(n).rounding = Rounding(v); });
        choice(
            "Overflow", "numericOverflow", {"Reject", "Saturate"},
            [policy](const auto &n) { return int(policy(n).overflow); },
            [policy](auto &n, int v) { policy(n).overflow = Overflow(v); });
        choice(
            "Precision loss", "numericPrecision", {"Reject inexact", "Allow declared"},
            [policy](const auto &n) { return int(policy(n).precision_loss); },
            [policy](auto &n, int v) { policy(n).precision_loss = PrecisionLoss(v); });
        choice(
            "Nonfinite values", "numericNonfinite", {"Reject", "Replace finite", "Clamp finite"},
            [policy](const auto &n) { return int(policy(n).nonfinite); },
            [policy](auto &n, int v) { policy(n).nonfinite = NonFinite(v); });
        section = Section::Additional;
        choice(
            "Scaling", "numericScaling",
            {"Identity", "Integer affine", "Dyadic affine", "PCM normalize", "PCM denormalize", "Binary64 affine"},
            [policy](const auto &n) { return int(policy(n).scaling.mode); },
            [policy](auto &n, int v) { policy(n).scaling.mode = ScaleMode(v); });
        literal(
            "Gain", "numericGain", [policy](const auto &n) { return policy(n).scaling.gain.numerator; },
            [policy](auto &n, auto v) { policy(n).scaling.gain.numerator = v; });
        literal(
            "Offset", "numericOffset", [policy](const auto &n) { return policy(n).scaling.offset.numerator; },
            [policy](auto &n, auto v) { policy(n).scaling.offset.numerator = v; });
        for (const bool gain : {true, false}) {
            integer(
                gain ? "Gain exponent (base 2)" : "Offset exponent (base 2)",
                gain ? "numericGainExponent" : "numericOffsetExponent", std::numeric_limits<std::int16_t>::min(),
                std::numeric_limits<std::int16_t>::max(),
                [gain, policy](const auto &n) {
                    const auto &scaling = policy(n).scaling;
                    return gain ? scaling.gain.exponent2 : scaling.offset.exponent2;
                },
                [gain, policy](auto &n, int v) {
                    auto &scaling = policy(n).scaling;
                    (gain ? scaling.gain : scaling.offset).exponent2 = std::int16_t(v);
                });
        }
        literal(
            "NaN replacement", "numericNan", [policy](const auto &n) { return policy(n).nan_replacement; },
            [policy](auto &n, auto v) { policy(n).nan_replacement = v; });
        literal(
            "Negative infinity replacement", "numericNegativeInf",
            [policy](const auto &n) { return policy(n).negative_inf_replacement; },
            [policy](auto &n, auto v) { policy(n).negative_inf_replacement = v; });
        literal(
            "Positive infinity replacement", "numericPositiveInf",
            [policy](const auto &n) { return policy(n).positive_inf_replacement; },
            [policy](auto &n, auto v) { policy(n).positive_inf_replacement = v; });
    }

    auto reset = makeField("Reset node settings", "numericResetSettings", Editor::Action);
    reset.write = [this, _kind](const Value &) {
        auto n = numeric_editor::default_node(_kind);
        n.id = _node.id;
        n.enabled = _node.enabled;
        if (_kind == "ScalarView")n.inputs=_node.inputs;
        if (_kind == "InputSelector") {
            n.inputs = _node.inputs;
            n.outputs = _node.outputs;
            for (auto &p : std::get<TransformProcessingParameters>(n.parameters).values)
                if (p.key == "selected_input")
                    p.value = NumericLiteral{std::int64_t(n.inputs.front().id)};
        }
        return commit(std::move(n));
    };
    _fields.push_back(std::move(reset));
    auto readAdapter = [this](QString key, bool endpoint) {
        return endpoint ? _adapter.value("remote_endpoint").toObject().value(key) : _adapter.value(key);
    };
    auto writeAdapter = [this](QString key, bool endpoint, QJsonValue value) {
        auto adapter = _adapter;
        if (endpoint) {
            auto remote = adapter.value("remote_endpoint").toObject();
            if (value.isUndefined())
                remote.remove(key);
            else
                remote[key] = value;
            if (remote.isEmpty())
                adapter.remove("remote_endpoint");
            else
                adapter["remote_endpoint"] = remote;
        } else if (value.isUndefined())
            adapter.remove(key);
        else
            adapter[key] = value;
        return adapter == _adapter || (_edit_adapter && _edit_adapter(adapter));
    };
    auto adapterText = [&](const char *label, const char *field, const char *name, bool endpoint = false) {
        auto f = makeField(label, name, Editor::Text);
        f.read = [readAdapter, field, endpoint]() -> Value { return readAdapter(field, endpoint).toString(); };
        f.write = [writeAdapter, field, endpoint](const Value &v) {
            const auto t = std::get<QString>(v);
            return writeAdapter(field, endpoint, t.isEmpty() ? QJsonValue(QJsonValue::Undefined) : QJsonValue(t));
        };
        _fields.push_back(std::move(f));
    };
    auto adapterNumber = [&](const char *label, const char *field, const char *name, int minimum, int maximum,
                             bool endpoint = false) {
        auto f = makeField(label, name, Editor::Integer);
        f.minimum = minimum;
        f.maximum = maximum;
        f.read = [readAdapter, field, minimum, endpoint]() -> Value {
            return readAdapter(field, endpoint).toInt(minimum);
        };
        f.write = [writeAdapter, field, minimum, endpoint](const Value &v) {
            const auto number = std::get<int>(v);
            return writeAdapter(field, endpoint,
                                number == minimum ? QJsonValue(QJsonValue::Undefined) : QJsonValue(number));
        };
        _fields.push_back(std::move(f));
    };
    if (_kind == "NetworkInput") {
        adapterText("Provider UUID", "provider_uuid", "adapterProviderUuid");
        adapterText("Stream ID", "stream_id", "adapterStreamId");
        adapterText("Remote host", "host", "adapterHost");
        adapterNumber("Remote service port", "port", "adapterPort", 0, 65535);
        adapterText("Device identity", "device_identity", "adapterDeviceIdentity");
        adapterText("Remote graph ID", "graph_id", "adapterGraphId", true);
        adapterNumber("Remote node ID", "node_id", "adapterNodeId", -1, std::numeric_limits<int>::max(), true);
        adapterNumber("Remote port index", "port_index", "adapterPortIndex", -1, 65535, true);
        auto f = makeField("Remote port direction", "adapterDirection", Editor::Choice);
        f.options = {{"Unspecified", 0}, {"Input port", 1}, {"Output port", 2}};
        f.read = [readAdapter]() -> Value {
            const auto v = readAdapter("direction", true).toString();
            return v == "input" ? 1 : v == "output" ? 2 : 0;
        };
        f.write = [writeAdapter](const Value &v) {
            const auto i = std::get<int>(v);
            return writeAdapter("direction", true,
                                i == 1   ? QJsonValue("input")
                                : i == 2 ? QJsonValue("output")
                                         : QJsonValue(QJsonValue::Undefined));
        };
        _fields.push_back(std::move(f));
    }
    if (_kind == "AudioSource") {
        auto f = makeField("Capture device", "captureDevice", Editor::Choice);
        f.options.push_back({"Follow system default", QString{}, true});
        QString error;
        const auto choices =
            _capture_devices ? _capture_devices(numeric_editor::capture_source_kind(_node), &error) : QJsonArray{};
        for (const auto &v : choices) {
            const auto c = v.toObject();
            if (!c.value("id").toString().isEmpty())
                f.options.push_back({c.value("name").toString(), c.value("id").toString(), false});
        }
        const auto selected = _adapter.value("device_identity").toString();
        bool found = false;
        for (const auto &o : f.options)
            if (std::get<QString>(o.value) == selected)
                found = true;
        if (!found) {
            f.options.push_back({"Unavailable device: %1", selected, true});
            if (error.isEmpty())
                error = "Selected capture device is unavailable";
        }
        f.read = [this]() -> Value { return _adapter.value("device_identity").toString(); };
        f.write = [writeAdapter](const Value &v) {
            const auto t = std::get<QString>(v);
            return writeAdapter("device_identity", false,
                                t.isEmpty() ? QJsonValue(QJsonValue::Undefined) : QJsonValue(t));
        };
        _fields.push_back(std::move(f));
        auto status = makeField("Capture device status", "captureDeviceError", Editor::Caption);
        status.pinnable = false;
        status.translate_value = true;
        status.read = [error]() -> Value { return error; };
        _fields.push_back(std::move(status));
        auto refresh = makeField("Refresh devices", "refreshCaptureDevices", Editor::Action);
        refresh.write = [this](const Value &) {
            buildFields();
            emit valuesChanged();
            return true;
        };
        _fields.push_back(std::move(refresh));
    }
}
