#include <QApplication>
#include <QtTest/QTest>
#include <QtNodes/GraphicsView>
#include <QtNodes/DataFlowGraphModel>
#include <QtNodes/DataFlowGraphicsScene>
#include <QtNodes/NodeDelegateModelRegistry>
int main(int argc, char **argv) {
    QApplication app(argc, argv);
    auto registry = std::make_shared<QtNodes::NodeDelegateModelRegistry>();
    QtNodes::DataFlowGraphModel graph(registry);
    QtNodes::DataFlowGraphicsScene scene(graph);
    scene.addRect(0,0,200,100);
    QtNodes::GraphicsView view(&scene);
    view.resize(800,600); view.show(); QTest::qWait(30);
    view.setupScale(0.75); view.centerOn(1200,900);
    const auto before = view.mapToScene(view.viewport()->rect().center());
    view.hide(); view.show(); QTest::qWait(30);
    const auto after = view.mapToScene(view.viewport()->rect().center());
    qInfo("Pinned QtNodes hide/show center: (%g,%g) -> (%g,%g), scale %g",
          before.x(),before.y(),after.x(),after.y(),view.getScale());
    return QLineF(before,after).length()>100 ? 0 : 1; // Reproduction, not adapter correctness.
}
