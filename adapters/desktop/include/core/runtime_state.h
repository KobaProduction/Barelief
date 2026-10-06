#pragma once
#include <QMetaType>
namespace core { enum class RuntimeState { Stopped, Starting, Running, Recovering, Failed }; }
Q_DECLARE_METATYPE(core::RuntimeState)
