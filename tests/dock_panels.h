#pragma once
#include <QDockWidget>
#include <QMainWindow>
#include <array>
// Test convenience only: exercise dock panels with the old index-to-view mapping.
class DockPanels {
    QMainWindow *_window; int _current = 4;
    const std::array<const char *,7> _ids{"rawSamplesDock","windowSignalDock","windowedSignalDock","windowFunctionDock","fftDock","melDock","spectrogramDock"};
  public:
    explicit DockPanels(QMainWindow *window) : _window(window) {}
    QDockWidget *dock(int index) const { return _window->findChild<QDockWidget *>(_ids.at(index)); }
    bool isTabVisible(int index) const { return !dock(index)->isHidden(); }
    void setTabVisible(int index, bool visible) { dock(index)->setVisible(visible); }
    void setCurrentIndex(int index) { _current=index; dock(index)->raise(); }
    int currentIndex() const { return _current; }
    int count() const { return int(_ids.size()); }
    QWidget *widget(int index) const { return dock(index)->widget(); }
};
