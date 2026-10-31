import os
import unittest

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))

class TestT1bArenaRelease(unittest.TestCase):
    def test_guest_arena_hpp_declares_wait_for_release(self):
        header_path = os.path.join(REPO_ROOT, 'src', 'core', 'memory', 'GuestArena.hpp')
        with open(header_path, 'r', encoding='utf-8') as f:
            content = f.read()
        self.assertIn('static bool waitForRelease', content)
        self.assertIn('static uint32_t liveCount()', content)

    def test_guest_arena_cpp_implements_wait_for_release(self):
        cpp_path = os.path.join(REPO_ROOT, 'src', 'core', 'memory', 'GuestArena.cpp')
        with open(cpp_path, 'r', encoding='utf-8') as f:
            content = f.read()
        self.assertIn('bool GuestArena::waitForRelease(double timeoutSeconds, uint32_t targetCount) noexcept', content)
        self.assertIn('liveArenas.fetch_sub(1, std::memory_order_release)', content)
        self.assertIn('liveArenas.load(std::memory_order_acquire)', content)

    def test_hvm_engine_bridge_exposes_arena_helpers(self):
        h_path = os.path.join(REPO_ROOT, 'app', 'HydrogenVM', 'Bridge', 'HVMEngine.h')
        with open(h_path, 'r', encoding='utf-8') as f:
            h_content = f.read()
        self.assertIn('+ (BOOL)waitForGuestArenaRelease:(NSTimeInterval)timeout;', h_content)
        self.assertIn('+ (NSUInteger)liveGuestArenaCount;', h_content)

        mm_path = os.path.join(REPO_ROOT, 'app', 'HydrogenVM', 'Bridge', 'HVMEngine.mm')
        with open(mm_path, 'r', encoding='utf-8') as f:
            mm_content = f.read()
        self.assertIn('+ (BOOL)waitForGuestArenaRelease:(NSTimeInterval)timeout', mm_content)
        self.assertIn('+ (NSUInteger)liveGuestArenaCount', mm_content)

    def test_batch_test_runner_waits_for_arena_release(self):
        swift_path = os.path.join(REPO_ROOT, 'app', 'HydrogenVM', 'BatchTest.swift')
        with open(swift_path, 'r', encoding='utf-8') as f:
            swift_content = f.read()
        self.assertIn('HVMEngine.waitForGuestArenaRelease(8.0)', swift_content)
        self.assertIn('HVMEngine.liveGuestArenaCount() == 0', swift_content)
        self.assertIn('HVMEngine.waitForGuestArenaRelease(5.0)', swift_content)

if __name__ == '__main__':
    unittest.main()
