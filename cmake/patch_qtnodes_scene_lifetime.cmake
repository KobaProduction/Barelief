# Bind the pinned QtNodes scene callback to its QObject lifetime. The graph
# can outlive a temporary scene (clipboard/editor replacement).
set(scene_path "${qtnodes_SOURCE_DIR}/src/DataFlowGraphicsScene.cpp")
file(READ "${scene_path}" scene_content)
string(REPLACE "\r\n" "\n" scene_content "${scene_content}")
set(scene_anchor [=[            &DataFlowGraphModel::inPortDataWasSet,
            [this](NodeId const nodeId, PortType const, PortIndex const) { onNodeUpdated(nodeId); });]=])
set(scene_replacement [=[            &DataFlowGraphModel::inPortDataWasSet,
            this,
            [this](NodeId const nodeId, PortType const, PortIndex const) { onNodeUpdated(nodeId); });]=])
string(FIND "${scene_content}" "${scene_replacement}" scene_patched)
if(scene_patched EQUAL -1)
    string(FIND "${scene_content}" "${scene_anchor}" scene_found)
    if(scene_found EQUAL -1)
        message(FATAL_ERROR "Pinned QtNodes scene lifetime patch no longer matches")
    endif()
    string(REPLACE "${scene_anchor}" "${scene_replacement}" scene_content "${scene_content}")
    file(WRITE "${scene_path}" "${scene_content}")
endif()
