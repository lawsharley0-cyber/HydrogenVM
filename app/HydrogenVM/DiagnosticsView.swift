import SwiftUI

/// On-device checks: can we reserve the 4 GB guest arena (entitlement), and
/// does the CPU core run correctly and how fast.
struct DiagnosticsView: View {
    struct TestOutcome: Sendable { let passed: Bool; let mips: Double; let detail: String }

    @State private var arenaOK: Bool?
    @State private var arenaError = ""
    @State private var running = false
    @State private var outcome: TestOutcome?

    var body: some View {
        NavigationStack {
            Form {
                Section("Guest memory") {
                    LabeledContent("4 GB arena") {
                        switch arenaOK {
                        case .none: Text("…")
                        case .some(true): Label("Available", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
                        case .some(false): Label("Unavailable", systemImage: "xmark.octagon.fill").foregroundStyle(.red)
                        }
                    }
                    if arenaOK == false {
                        Text(arenaError).font(.footnote).foregroundStyle(.secondary)
                    }
                }
                Section("CPU core") {
                    Button(running ? "Running…" : "Run self-test") { runSelfTest() }
                        .disabled(running || arenaOK != true)
                    if let o = outcome {
                        LabeledContent("Result", value: o.passed ? "Passed" : "Failed")
                        LabeledContent("Speed", value: String(format: "%.0f MIPS", o.mips))
                        Text(o.detail).font(.footnote.monospaced()).foregroundStyle(.secondary)
                    }
                }
            }
            .navigationTitle("Diagnostics")
            .task {
                var err: NSString?
                arenaOK = HVMEngine.canReserveGuestArena(&err)
                arenaError = (err as String?) ?? ""
            }
        }
    }

    private func runSelfTest() {
        running = true
        Task {
            let o = await Task.detached(priority: .userInitiated) { () -> TestOutcome in
                let r = HVMEngine.runSelfTest()
                return TestOutcome(passed: r.passed, mips: r.mips, detail: r.detail)
            }.value
            outcome = o
            running = false
        }
    }
}
