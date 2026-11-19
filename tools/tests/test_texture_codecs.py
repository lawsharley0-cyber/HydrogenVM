"""Compare the Android CPU texture decoders against synthetic reference vectors."""
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


DRIVER = r"""
#include "runtime/graphics/TextureCodecs.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 5) return 2;
    const auto format = std::strtoul(argv[1], nullptr, 16);
    const int width = std::atoi(argv[2]), height = std::atoi(argv[3]);
    std::string input = argv[4];
    std::vector<uint8_t> data;
    for (size_t i = 0; i < input.size(); i += 2)
        data.push_back(std::strtoul(input.substr(i, 2).c_str(), nullptr, 16));
    auto image = Hydrogen::Graphics::decodeCompressedTexture(format, width, height, data);
    if (!image) { std::puts("rejected"); return 0; }
    std::printf("%d %d\n", image->components, image->srgb);
    for (uint8_t pixel : image->rgba) std::printf("%02x", unsigned(pixel));
    for (float sample : image->redGreen) std::printf("%.9g ", double(sample));
    std::puts("");
}
"""


class TextureCodecsTest(unittest.TestCase):
    def test_reference_pixels(self):
        src = Path(__file__).resolve().parents[2] / "src"
        fixtures = src / "runtime/graphics/TextureCodecs.fixtures.json"
        cases = json.loads(fixtures.read_text())["cases"]
        with tempfile.TemporaryDirectory() as tmp:
            driver = Path(tmp) / "decode.cpp"
            driver.write_text(DRIVER)
            executable = Path(tmp) / "decode"
            subprocess.run([
                *shlex.split(os.environ.get("CXX", "c++")), "-std=c++20", "-O2",
                *shlex.split(os.environ.get("CXXFLAGS", "")), "-I", src, driver,
                src / "runtime/graphics/TextureCodecs.cpp", src / "runtime/android/Etc1.cpp",
                "-o", executable,
            ], check=True)
            for case in cases:
                with self.subTest(case=case["name"]):
                    result = subprocess.check_output([
                        executable, case["format"], str(case["width"]), str(case["height"]), case["input"],
                    ], text=True).splitlines()
                    if case.get("rejected"):
                        self.assertEqual(result, ["rejected"])
                    elif "rgba" in case:
                        srgb = case["format"] in ("9275", "9277", "9279", "8c4c", "8c4d", "8c4e", "8c4f")
                        self.assertEqual(result, [f"4 {int(srgb)}", case["rgba"]])
                    else:
                        components = len(case["numerators"]) // (case["width"] * case["height"])
                        self.assertEqual(result[0], f"{components} 0")
                        samples = list(map(float, result[1].split()))
                        self.assertEqual(len(samples), len(case["numerators"]))
                        for actual, expected in zip(samples, case["numerators"]):
                            self.assertAlmostEqual(actual, expected / case["scale"], places=7)
        print(f"{len(cases)} synthetic texture cases")


if __name__ == "__main__":
    unittest.main()
