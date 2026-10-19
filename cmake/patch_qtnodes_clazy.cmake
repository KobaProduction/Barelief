# Qt 6 meta-object types must retain their namespace in generated signatures.
include("${CMAKE_CURRENT_LIST_DIR}/patch_dependency_source.cmake")
set(qtnodes_headers "${qtnodes_SOURCE_DIR}/include/QtNodes/internal")
foreach(method IN ITEMS connectionCreated connectionDeleted)
    patch_dependency_source("${qtnodes_headers}" AbstractGraphModel.hpp
        "void ${method}(ConnectionId const connectionId);"
        "void ${method}(QtNodes::ConnectionId const connectionId);")
endforeach()
foreach(method IN ITEMS inputConnectionCreated inputConnectionDeleted outputConnectionCreated outputConnectionDeleted)
    patch_dependency_source("${qtnodes_headers}" NodeDelegateModel.hpp
        "void ${method}(ConnectionId const &)"
        "void ${method}(QtNodes::ConnectionId const &)")
endforeach()
patch_dependency_source("${qtnodes_headers}" BasicGraphicsScene.hpp
    "void modified(BasicGraphicsScene *);" "void modified(QtNodes::BasicGraphicsScene *);")
foreach(method IN ITEMS connectionHovered connectionHoverLeft onConnectionDeleted onConnectionCreated)
    patch_dependency_source("${qtnodes_headers}" BasicGraphicsScene.hpp
        "void ${method}(ConnectionId const connectionId"
        "void ${method}(QtNodes::ConnectionId const connectionId")
endforeach()
foreach(argument IN ITEMS "QString name" "QString groupName")
    patch_dependency_source("${qtnodes_headers}" BasicGraphicsScene.hpp
        "${argument} = QStringLiteral(\"\")" "${argument} = QLatin1String(\"\")")
endforeach()
# Save writes an external file without mutating the scene. Keep its const C++
# API and meta-object invocability; it is an action rather than a receiving slot.
patch_dependency_source("${qtnodes_headers}" DataFlowGraphicsScene.hpp
    [=[public Q_SLOTS:
    bool save() const;
    bool load();]=]
    [=[public:
    Q_INVOKABLE bool save() const;

public Q_SLOTS:
    bool load();]=])
foreach(pair IN ITEMS
        "20a5b084|165, 176, 132, 32"
        "2083a4af|131, 164, 175, 32"
        "3fe0bebc|224, 190, 188, 63"
        "3feecdcb|238, 205, 203, 63"
        "eeffa500|255, 165, 0, 238"
        "eeaaaaaa|170, 170, 170, 238")
    string(REPLACE "|" ";" parts "${pair}")
    list(GET parts 0 argb)
    list(GET parts 1 rgba)
    patch_dependency_source("${qtnodes_headers}" GroupGraphicsObject.hpp
        "QColor(\"#${argb}\")" "QColor(${rgba})")
endforeach()
