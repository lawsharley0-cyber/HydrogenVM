#include "loader/Plist.hpp"
#include <cstdio>
#include <string>
#include <vector>

using namespace Hydrogen::Plist;

static int passed = 0;
static int failed = 0;

void check(bool condition, const std::string& msg) {
    if (!condition) {
        std::printf("FAIL %s\n", msg.c_str());
        ++failed;
    } else {
        std::printf("PASS %s\n", msg.c_str());
        ++passed;
    }
}

int main() {
    // 1. Basic XML string parsing
    {
        std::string xml = R"(<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Name</key>
    <string>TestApp</string>
    <key>Version</key>
    <integer>1</integer>
    <key>Debug</key>
    <true/>
</dict>
</plist>
)";
        std::vector<uint8_t> data(xml.begin(), xml.end());
        ValuePtr p = parse(data);
        check(p != nullptr, "XML: parsed successfully");
        check(p && p->type == Value::Type::Dict, "XML: root is dict");
        check(p && p->getString("Name") == "TestApp", "XML: string value");
        const Value* ver = p ? p->get("Version") : nullptr;
        check(ver && ver->type == Value::Type::Integer && ver->i == 1, "XML: integer value");
        const Value* dbg = p ? p->get("Debug") : nullptr;
        check(dbg && dbg->type == Value::Type::Bool && dbg->b == true, "XML: bool true value");

        // Output XML test
        if (p) {
            std::string outXml = toXml(*p);
            check(outXml.find("<key>Name</key>") != std::string::npos, "toXml: has Name key");
            check(outXml.find("<string>TestApp</string>") != std::string::npos, "toXml: has TestApp string");

            // Re-parse output XML
            std::vector<uint8_t> outData(outXml.begin(), outXml.end());
            ValuePtr p2 = parse(outData);
            check(p2 && p2->getString("Name") == "TestApp", "toXml: output parses back successfully");
        }
    }

    // 2. OpenStep / .strings format parsing
    {
        std::string openstep = R"(
            {
                "Key1" = "Value1";
                Key2 = Value2; // comment
                /* block comment */
                "Key3" = (1, 2, "3");
            }
        )";
        std::vector<uint8_t> data(openstep.begin(), openstep.end());
        ValuePtr p = parse(data);
        check(p != nullptr, "OpenStep: parsed successfully");
        check(p && p->type == Value::Type::Dict, "OpenStep: root is dict");
        check(p && p->getString("Key1") == "Value1", "OpenStep: quoted key and value");
        check(p && p->getString("Key2") == "Value2", "OpenStep: bare key and value");

        const Value* arr = p ? p->get("Key3") : nullptr;
        check(arr && arr->type == Value::Type::Array && arr->array.size() == 3, "OpenStep: array parsed");
    }

    // 3. Error conditions
    {
        bool threw = false;
        try {
            std::string invalid = "<plist><dict><key>MissingValue</key></dict></plist>";
            std::vector<uint8_t> data(invalid.begin(), invalid.end());
            parse(data);
        } catch (const PlistError&) {
            threw = true;
        }
        check(threw, "XML: throws on missing value for key");

        threw = false;
        try {
            std::string invalid = "{ Key = ; }";
            std::vector<uint8_t> data(invalid.begin(), invalid.end());
            parse(data);
        } catch (const PlistError&) {
            threw = true;
        }
        check(threw, "OpenStep: throws on syntax error");
    }

    std::printf("Plist metadata: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
