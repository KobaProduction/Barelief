#pragma once
#include "pipeline/typed_frame.h"
#include <span>
namespace pipeline::stream {
using Frame = TypedFrame;
using Type = FrameType;
constexpr size_t MaxFrameBytes = 2 * 1024 * 1024;
bool valid(const Frame &frame);
size_t payload_size(const Frame &frame);
std::vector<uint8_t> encode(const Frame &frame);
bool decode(std::span<const uint8_t> bytes, Frame &frame, std::string &error);
} // namespace pipeline::stream
