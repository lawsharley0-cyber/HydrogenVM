import os
import unittest

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))

class TestG4HostFault(unittest.TestCase):
    def test_engine_declares_host_fault_report(self):
        header_path = os.path.join(REPO_ROOT, 'src', 'core', 'cpu', 'Engine.hpp')
        with open(header_path, 'r', encoding='utf-8') as f:
            content = f.read()
        self.assertIn('static void writeHostFaultReport(int sig, const siginfo_t* si, int fd);', content)
        self.assertIn('static void recordLogLineAsyncSafe', content)
        self.assertIn('static void setFaultReportFd', content)

    def test_engine_implements_host_fault_diagnostics(self):
        cpp_path = os.path.join(REPO_ROOT, 'src', 'core', 'cpu', 'Engine.cpp')
        with open(cpp_path, 'r', encoding='utf-8') as f:
            content = f.read()
        self.assertIn('void Engine::writeHostFaultReport', content)
        self.assertIn('g_activeArena', content)
        self.assertIn('=== HVM HOST FAULT DIAGNOSTICS ===', content)
        self.assertIn('--- Guest Threads State ---', content)
        self.assertIn('--- Recent Logs (last 200) ---', content)
        self.assertIn('=== END HOST FAULT DIAGNOSTICS ===', content)

    def test_guest_arena_supports_configurable_host_page(self):
        header_path = os.path.join(REPO_ROOT, 'src', 'core', 'memory', 'GuestArena.hpp')
        with open(header_path, 'r', encoding='utf-8') as f:
            content = f.read()
        self.assertIn('explicit GuestArena(size_t hostPage = 0);', content)

        cpp_path = os.path.join(REPO_ROOT, 'src', 'core', 'memory', 'GuestArena.cpp')
        with open(cpp_path, 'r', encoding='utf-8') as f:
            cpp_content = f.read()
        self.assertIn('GuestArena::GuestArena(size_t hostPage)', cpp_content)

    def test_batch_test_runner_opens_signal_file(self):
        swift_path = os.path.join(REPO_ROOT, 'app', 'HydrogenVM', 'BatchTest.swift')
        with open(swift_path, 'r', encoding='utf-8') as f:
            content = f.read()
        self.assertIn('HVMCrashJournal.openSignalFile(CrashReport.signal.path)', content)
        self.assertIn('HVMCrashJournal.closeSignalFile()', content)

    def test_hvm_engine_records_async_safe_logs(self):
        mm_path = os.path.join(REPO_ROOT, 'app', 'HydrogenVM', 'Bridge', 'HVMEngine.mm')
        with open(mm_path, 'r', encoding='utf-8') as f:
            content = f.read()
        self.assertIn('Hydrogen::CPU::Engine::recordLogLineAsyncSafe(msg.data(), msg.size());', content)

if __name__ == '__main__':
    unittest.main()
