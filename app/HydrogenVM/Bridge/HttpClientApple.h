// HydrogenVM — the iPhone's HTTP client (HttpClientApple.mm). See src/runtime/libc/HttpClient.hpp.
#pragma once
#include <memory>
#include "runtime/libc/HttpClient.hpp"

namespace Hydrogen::App {
std::shared_ptr<Hydrogen::Runtime::HttpClient> makeAppleHttpClient();
}
