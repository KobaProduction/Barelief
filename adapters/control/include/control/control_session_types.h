#pragma once
#include <cstdint>
#include <cstddef>

namespace control {
// BDSP transport envelope for numeric control.
inline constexpr std::uint8_t ControlHelloId=21, ControlHelloAckId=22,
    ControlCommandId=23, ControlReplyId=24, ControlFragmentId=25;
inline constexpr std::size_t ControlFragmentHeaderBytes=29, ControlFragmentBytes=60000;
enum class ControlSessionState { Disconnected, Negotiating, Ready, Failed };
enum class ControlSessionError { None, InvalidConfiguration, Malformed, Unsupported,
    WrongDevice, ReadOnly, Busy, LimitExceeded, Timeout, Disconnected, Identity,
    Replay, Mapping, Codec, Transport };
}
