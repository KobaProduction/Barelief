#ifndef AGGREGATOR_UTILS_H
#define AGGREGATOR_UTILS_H

#include "abstract/types.h"

namespace aggregator_utils {

/// @brief Преобразует состояние компонента в текстовое представление
/// @param state Состояние компонента
/// @return Строковое представление состояния
QString component_state_to_string(ComponentState state);

/// @brief Преобразует тип компонента в текстовое представление
/// @param type Тип компонента
/// @return Строковое представление типа
QString component_type_to_string(ComponentType type);

/// @brief Генерирует токен сессии на основе данных компонента
/// @param uuid UUID компонента
/// @param type Тип компонента
/// @param component_server_port Порт сервера компонента
/// @return CRC16 токен сессии
session_token_t generate_session_token(const QUuid &uuid, ComponentType type, uint16_t component_server_port);

/// @brief Форматирует информацию о компоненте для логирования
/// @param uuid UUID компонента
/// @param type Тип компонента
/// @param state Состояние компонента
/// @return Отформатированная строка
QString format_component_info(const QUuid &uuid, ComponentType type, ComponentState state);

/// @brief Форматирует изменение состояния компонента для логирования
/// @param uuid UUID компонента
/// @param new_state Новое состояние
/// @return Отформатированная строка
QString format_component_state_change(const QUuid &uuid, ComponentState new_state);

/// @brief Форматирует информацию о регистрации компонента для логирования
/// @param uuid UUID компонента
/// @param type Тип компонента
/// @param address Адрес подключения
/// @return Отформатированная строка
QString format_component_registration(const QUuid &uuid, ComponentType type, const QString &address);

} // namespace aggregator_utils

#endif // AGGREGATOR_UTILS_H