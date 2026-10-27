#include "runtime/MemoryTelemetry.hpp"
#include <iostream>
#include <thread>
#include <vector>
#include <stdexcept>
#include <string>

using namespace Hydrogen::Runtime;

void check(bool cond, const char* msg) {
    if (!cond) {
        std::cerr << "FAILED: " << msg << std::endl;
        throw std::runtime_error(msg);
    }
}

void runTests() {
    // 1. Basic initialization and state/counter registration
    {
        MemoryTelemetry t("test.bundle", "");
        uint8_t s = t.registerState("running");
        t.setState(s);

        TelemetryCounter c1 = t.registerCounter("c1");
        t.updateCounter(c1, 100);

        std::string json = t.reportJson();
        check(json.find("\"test.bundle\"") != std::string::npos, "Bundle ID missing");
        check(json.find("\"c1\"") != std::string::npos, "Counter name missing");
        check(json.find("\"running\"") != std::string::npos, "State name missing");
    }

    // 2. Concurrent counter updates
    {
        MemoryTelemetry t("concurrent", "");
        TelemetryCounter c1 = t.registerCounter("test_counter");

        std::vector<std::thread> threads;
        for (int i = 0; i < 10; ++i) {
            threads.push_back(std::thread([&t, c1]() {
                for (int j = 0; j < 1000; ++j) {
                    t.updateCounter(c1, j);
                }
            }));
        }
        for (auto& th : threads) th.join();
        t.updateCounter(c1, 42);
        std::string json = t.reportJson();
        check(json.find("test_counter") != std::string::npos, "Concurrent missing counter");
    }

    // 3. Thread lifecycle and sampling
    {
        MemoryTelemetry t("sampling", "");
        t.registerCounter("A");
        t.registerState("S");
        t.start();
        std::this_thread::sleep_for(std::chrono::milliseconds(1100)); // allow a sample
        t.stop();
        std::string r = t.reportJson();
        check(r.find("\"samples\":[{") != std::string::npos, "Samples array not populated");
    }

    std::cout << "Telemetry tests passed." << std::endl;
}

int main() {
    try {
        runTests();
    } catch (const std::exception& e) {
        return 1;
    }
    return 0;
}
