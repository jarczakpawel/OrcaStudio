#pragma once

#include "RuntimeCoreTypes.hpp"

namespace SlicerLinuxRuntimeCore {

std::string encode_rpc_frame(const RpcFrame& frame);
bool decode_rpc_frame(const std::string& line, RpcFrame& frame, std::string& error);

std::string encode_runtime_event(const RuntimeEvent& event);
bool decode_runtime_event(const std::string& line, RuntimeEvent& event, std::string& error);

}
