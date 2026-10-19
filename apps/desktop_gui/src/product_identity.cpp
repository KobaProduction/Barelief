#include <core_app/product_identity.h>
#include <QResource>
#include <QPainter>
#include <QSvgRenderer>

// Explicit initialization retains resources when linked from the static composition library.
static void initialize_product_resources() {
    Q_INIT_RESOURCE(barelief_branding);
    Q_INIT_RESOURCE(barelief_licenses);
}
void barelief::initialize_resources() {
    initialize_product_resources();
}
QIcon barelief::application_icon() {
    QSvgRenderer renderer(QStringLiteral(":/barelief/barelief.svg"));
    QIcon icon;
    for(const auto size:{16,20,24,32,40,48,64,96,128,256,512}){
        QPixmap image(size,size);image.fill(Qt::transparent);
        QPainter painter(&image);renderer.render(&painter);painter.end();
        icon.addPixmap(image);
    }
    return icon;
}
