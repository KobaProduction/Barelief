# ADS 5.1.1 has no virtual/configurable hit-test hook. Keep a narrow, checked
# source patch: exact indicators win; the remaining target uses broad bands.
set(source "${ADS_SOURCE}/src/DockOverlay.cpp")
file(READ "${source}" content)
string(REPLACE "\r\n" "\n" content "${content}")
if(NOT content MATCHES "Application invisible docking indicators")
    set(indicator "\t\tQWidget* widget = i2.value();")
    string(FIND "${content}" "${indicator}" indicatorFound)
    if(indicatorFound EQUAL -1)
        message(FATAL_ERROR "ADS indicator patch no longer matches the pinned source")
    endif()
    string(REPLACE "#include \"DockOverlay.h\"" "#include \"DockOverlay.h\"\n#include <QGraphicsOpacityEffect>" content "${content}")
    string(REPLACE "${indicator}" "${indicator}\n        // Application invisible docking indicators: retain hit geometry.\n        auto *effect = new QGraphicsOpacityEffect(widget);\n        effect->setOpacity(0); widget->setGraphicsEffect(effect);" content "${content}")
endif()
if(content MATCHES "Application broad drop bands")
    file(WRITE "${source}" "${content}")
    return()
endif()
set(anchor "\treturn InvalidDockWidgetArea;\n}\n\n\n//============================================================================\nvoid CDockOverlayCross::showEvent")
set(replacement [=[
    // Application broad drop bands. Indicators retain priority above.
    if (rect().contains(pos) && width() > 0 && height() > 0)
    {
        const double x = double(pos.x()) / width();
        const double y = double(pos.y()) / height();
        const double distances[] = {x, 1.0-x, y, 1.0-y};
        const DockWidgetArea areas[] = {LeftDockWidgetArea, RightDockWidgetArea,
                                       TopDockWidgetArea, BottomDockWidgetArea};
        int nearest = 0;
        for (int j = 1; j < 4; ++j)
            if (distances[j] < distances[nearest]) nearest = j;
        const double band = d->Mode == CDockOverlay::ModeDockAreaOverlay ? 0.22 : 0.10;
        if (distances[nearest] < band && d->DockOverlay->allowedAreas().testFlag(areas[nearest]))
            return areas[nearest];
        if (d->Mode == CDockOverlay::ModeDockAreaOverlay &&
            d->DockOverlay->allowedAreas().testFlag(CenterDockWidgetArea))
            return CenterDockWidgetArea;
    }
    return InvalidDockWidgetArea;
}


//============================================================================
void CDockOverlayCross::showEvent]=])
string(FIND "${content}" "${anchor}" found)
if(found EQUAL -1)
    message(FATAL_ERROR "ADS drop-zone patch no longer matches the pinned source")
endif()
string(REPLACE "${anchor}" "${replacement}" content "${content}")
file(WRITE "${source}" "${content}")
