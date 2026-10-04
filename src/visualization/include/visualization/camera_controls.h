#ifndef VISUALIZATION_CAMERA_CONTROLS_H
#define VISUALIZATION_CAMERA_CONTROLS_H
#include <QJsonObject>
#include <QJsonArray>
#include <QKeyEvent>
#include <QKeySequence>

namespace CameraControls {
inline int scan(const QKeyEvent &event) {
    int value = event.nativeScanCode();
    if ((value & 0xff00) == 0xe000) value = (value & 0xff) | 0x100;
    if (value == 54) return 42; // Both Shift keys.
    if (value == 0x11d || value == 0xe01d) return 29; // Both Ctrl keys.
    return value;
}
inline int logical(int key) {
    if (key < 0 || key > 0xffff) return key;
    const QString russian = QString::fromUtf8("ЙЦУКЕНГШЩЗФЫВАПРОЛДЯЧСМИТЬ");
    const QString english = "QWERTYUIOPASDFGHJKLZXCVBNM";
    const auto index = russian.indexOf(QChar(key));
    return index >= 0 ? english[index].unicode() : key;
}
inline QString label(int scanCode, int key) {
    const int codes[] = {16,17,18,19,20,21,22,23,24,25,30,31,32,33,34,35,36,37,38,44,45,46,47,48,49,50};
    const QString letters = "QWERTYUIOPASDFGHJKLZXCVBNM";
    for (int i = 0; i < 26; ++i) if (scanCode == codes[i]) return QString(letters[i]);
    if (key == Qt::Key_Control || scanCode == 29) return "Ctrl";
    if (key == Qt::Key_Shift || scanCode == 42) return "Shift";
    if (key == Qt::Key_Space || scanCode == 57) return "Space";
    return QKeySequence(logical(key)).toString(QKeySequence::NativeText);
}
inline QJsonObject binding(int scanCode, int key) { return {{"scan",scanCode},{"key",logical(key)}}; }
inline QJsonObject defaults() {
    return {{"forward",QJsonArray{binding(17,Qt::Key_W)}}, {"backward",QJsonArray{binding(31,Qt::Key_S)}},
            {"left",QJsonArray{binding(30,Qt::Key_A)}}, {"right",QJsonArray{binding(32,Qt::Key_D)}},
            {"up",QJsonArray{binding(57,Qt::Key_Space)}}, {"down",QJsonArray{binding(29,Qt::Key_Control)}},
            {"fast",QJsonArray{binding(42,Qt::Key_Shift)}}, {"reset",QJsonArray{binding(44,Qt::Key_Z)}}};
}
inline QJsonObject normalized(const QJsonObject &saved) {
    auto result = defaults();
    for (auto it = result.begin(); it != result.end(); ++it) {
        if (!saved[it.key()].isArray()) continue;
        QJsonArray valid;
        for (const auto &value : saved[it.key()].toArray()) {
            const auto object = value.toObject();
            const int scanCode = object["scan"].toInt(-1), key = object["key"].toInt(-1);
            if (scanCode >= 0 && scanCode <= 0x1ff && key > 0 && key != Qt::Key_Escape && valid.size() < 2)
                valid.append(binding(scanCode,key));
        }
        if (!valid.isEmpty()) it.value() = valid;
    }
    return result;
}
inline bool matches(const QJsonObject &binding, const QKeyEvent &event) {
    if (scan(event) && binding["scan"].toInt()) return scan(event) == binding["scan"].toInt();
    return logical(event.key()) == binding["key"].toInt();
}
inline bool same(const QJsonObject &a, const QJsonObject &b) {
    if (a["scan"].toInt() && b["scan"].toInt()) return a["scan"] == b["scan"];
    return a["key"] == b["key"];
}
}
#endif
