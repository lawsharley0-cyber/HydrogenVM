#pragma once
#include <string>
#include <vector>

namespace Hydrogen::Loader {

// The engine an APK's game is built on, from its native library names and the classes its DEX
// names, and whether HydrogenVM has a driver for it ("driver", "planned", "none"). Kept in step
// with tools/android_gaps.py and runtime/android/App.cpp's drivers.
struct AndroidEngine { std::string name, support; };
AndroidEngine detectAndroidEngine(const std::vector<std::string>& libNames, const std::string& dexText);

} // namespace Hydrogen::Loader
