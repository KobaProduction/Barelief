# A connection may atomically adapt presentation-model contracts. The creation
# command owns its undo snapshot; ordinary disconnection retains current clocks.
# Keep this extension at the Qt adapter boundary, never in portable graph code.
function(qtnodes_connection_undo_replace relative anchor replacement marker)
    set(path "${qtnodes_SOURCE_DIR}/${relative}")
    file(READ "${path}" content)
    string(REPLACE "\r\n" "\n" content "${content}")
    string(FIND "${content}" "${marker}" patched)
    if(patched EQUAL -1)
        string(FIND "${content}" "${anchor}" found)
        if(found EQUAL -1)
            message(FATAL_ERROR "Pinned QtNodes connection undo patch no longer matches: ${relative}")
        endif()
        string(REPLACE "${anchor}" "${replacement}" content "${content}")
        file(WRITE "${path}" "${content}")
    endif()
endfunction()

# Clipboard and deletion undo must retain adapter-owned edge policy alongside IDs.
qtnodes_connection_undo_replace("include/QtNodes/internal/AbstractGraphModel.hpp"
    [=[    virtual QJsonObject saveNode(NodeId const) const { return {}; }]=]
    [=[    virtual QJsonObject saveNode(NodeId const) const { return {}; }
    virtual QJsonObject saveConnection(ConnectionId const&) const;
    virtual void loadConnection(QJsonObject const&);]=]
    "virtual QJsonObject saveConnection")

qtnodes_connection_undo_replace("src/AbstractGraphModel.cpp"
    [=[namespace QtNodes {]=]
    [=[namespace QtNodes {
QJsonObject AbstractGraphModel::saveConnection(ConnectionId const& id) const { return toJson(id); }
void AbstractGraphModel::loadConnection(QJsonObject const& value) { addConnection(fromJson(value)); }
]=]
    "AbstractGraphModel::saveConnection")

qtnodes_connection_undo_replace("src/UndoCommands.cpp"
    "connJsonArray.append(toJson(connectionId));"
    "connJsonArray.append(graphModel.saveConnection(connectionId));"
    "connJsonArray.append(graphModel.saveConnection(connectionId))")
qtnodes_connection_undo_replace("src/UndoCommands.cpp"
    "connJsonArray.append(toJson(cid));"
    "connJsonArray.append(graphModel.saveConnection(cid));"
    "connJsonArray.append(graphModel.saveConnection(cid))")
qtnodes_connection_undo_replace("src/UndoCommands.cpp"
    "graphModel.addConnection(connId);"
    "graphModel.loadConnection(connJson);"
    "graphModel.loadConnection(connJson)")

qtnodes_connection_undo_replace("include/QtNodes/internal/AbstractGraphModel.hpp"
    [=[    virtual bool deleteConnection(ConnectionId const connectionId) = 0;]=]
    [=[    virtual bool deleteConnection(ConnectionId const connectionId) = 0;

    virtual QJsonObject connectionUndoState(ConnectionId const) const { return {}; }
    virtual bool undoConnection(ConnectionId const connectionId, QJsonObject const&)
    { return deleteConnection(connectionId); }]=]
    "virtual QJsonObject connectionUndoState")

qtnodes_connection_undo_replace("include/QtNodes/internal/UndoCommands.hpp"
    [=[    ConnectionId _connId;
};

class NODE_EDITOR_PUBLIC MoveNodeCommand]=]
    [=[    ConnectionId _connId;
    QJsonObject _connectionUndoState;
};

class NODE_EDITOR_PUBLIC MoveNodeCommand]=]
    "QJsonObject _connectionUndoState;")

qtnodes_connection_undo_replace("src/UndoCommands.cpp"
    [=[ConnectCommand::ConnectCommand(BasicGraphicsScene *scene, ConnectionId const connId)
    : _scene(scene)
    , _connId(connId)
{
    //
}]=]
    [=[ConnectCommand::ConnectCommand(BasicGraphicsScene *scene, ConnectionId const connId)
    : _scene(scene)
    , _connId(connId)
    , _connectionUndoState(scene->graphModel().connectionUndoState(connId))
{
}]=]
    "_connectionUndoState(scene->graphModel().connectionUndoState(connId))")

qtnodes_connection_undo_replace("src/UndoCommands.cpp"
    [=[void ConnectCommand::undo()
{
    _scene->graphModel().deleteConnection(_connId);
}]=]
    [=[void ConnectCommand::undo()
{
    _scene->graphModel().undoConnection(_connId, _connectionUndoState);
}]=]
    "undoConnection(_connId, _connectionUndoState)")

# Preserve adapter-owned fields when pasted endpoint IDs are remapped.
qtnodes_connection_undo_replace("src/UndoCommands.cpp"
    "newConnJsonArray.append(toJson(newConnId));"
    [=[const auto remappedIds = toJson(newConnId);
        for (auto it = remappedIds.begin(); it != remappedIds.end(); ++it)
            connJson[it.key()] = it.value();
        newConnJsonArray.append(connJson);]=]
    "const auto remappedIds = toJson(newConnId)")
qtnodes_connection_undo_replace("src/UndoCommands.cpp"
    ".toInt()" ".toInteger()" "NodeId oldNodeId = nodeJson[\"id\"].toInteger()")
qtnodes_connection_undo_replace("include/QtNodes/internal/ConnectionIdUtils.hpp"
    ".toInt(InvalidNodeId)" ".toInteger(InvalidNodeId)" ".toInteger(InvalidNodeId)")
qtnodes_connection_undo_replace("include/QtNodes/internal/ConnectionIdUtils.hpp"
    ".toInt(InvalidPortIndex)" ".toInteger(InvalidPortIndex)" ".toInteger(InvalidPortIndex)")
