#pragma once

// Internal to Config.cpp, ConfigDispatch.cpp and Theme.cpp. Every variable here is
// DEFINED in Config.cpp, in its original order; these are only declarations.

#include "Config.hpp"
#include "FlightBlur.hpp"

#include "Compat.hpp"

#include <chrono>

#include <algorithm>
#include <config/shared/complex/ComplexDataType.hpp>
#include <config/shared/complex/ComplexDataTypes.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/ColorValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/config/values/types/GradientValue.hpp>
#include <hyprland/src/macros.hpp>
#include <hyprland/src/managers/KeybindManager.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <array>
#include <optional>
#include <regex>
#include <span>
#include <string>
#include <sys/stat.h>
#include <vector>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
}

namespace ConfigDetail {
    extern ScrollOverview::Config::TGestureRegistrar g_gestureRegistrar;
} // namespace ConfigDetail

namespace ScrollOverview::Config::ConfigPart {
    extern uint64_t g_themeGeneration;
    extern void registerLuaFunctions();
} // namespace ScrollOverview::Config::ConfigPart
