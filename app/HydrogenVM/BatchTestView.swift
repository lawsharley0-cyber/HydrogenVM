import SwiftUI
import UIKit

struct ShareSheet: UIViewControllerRepresentable {
    let items: [Any]

    init(items: [Any]) {
        self.items = items
    }

    func makeUIViewController(context: Context) -> UIActivityViewController {
        UIActivityViewController(activityItems: items, applicationActivities: nil)
    }

    func updateUIViewController(_ uiViewController: UIActivityViewController, context: Context) {}
}

struct BatchTestView: View {
    let installedApps: [InstalledApp]
    @State private var runner = BatchTestRunner()
    @State private var showOptionsSheet = false
    @State private var showShareSheet = false
    @State private var shareItems: [Any] = []
    @State private var copiedMarkdownAlert = false
    @Environment(\.dismiss) private var dismiss

    init(installedApps: [InstalledApp]) {
        self.installedApps = installedApps
    }

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                if runner.status == .running {
                    runningHeaderView
                }

                if runner.results.isEmpty && runner.status == .idle {
                    emptyStateView
                } else {
                    resultsListView
                }
            }
            .navigationTitle("Test All Games")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarLeading) {
                    Button("Close") {
                        if runner.status == .running {
                            runner.cancel()
                        }
                        dismiss()
                    }
                }
                ToolbarItem(placement: .topBarTrailing) {
                    HStack(spacing: 12) {
                        if !runner.results.isEmpty {
                            Button {
                                prepareShareItems()
                                showShareSheet = true
                            } label: {
                                Image(systemName: "square.and.arrow.up")
                            }

                            Button {
                                let md = runner.buildMarkdownReport()
                                UIPasteboard.general.string = md
                                copiedMarkdownAlert = true
                            } label: {
                                Image(systemName: "doc.on.doc")
                            }
                        }

                        Button {
                            showOptionsSheet = true
                        } label: {
                            Image(systemName: "gearshape")
                        }
                        .disabled(runner.status == .running)
                    }
                }
            }
            .sheet(isPresented: $showOptionsSheet) {
                optionsFormSheet
            }
            .sheet(isPresented: $showShareSheet) {
                ShareSheet(items: shareItems)
            }
            .alert("Markdown Copied", isPresented: $copiedMarkdownAlert) {
                Button("OK") {}
            } message: {
                Text("The batch test markdown report has been copied to your clipboard.")
            }
            .onAppear {
                if BatchTestRunner.hasPendingSession() && runner.status == .idle {
                    runner.resumePendingSession(allApps: installedApps)
                }
            }
        }
    }

    // MARK: - Running Header

    private var runningHeaderView: some View {
        VStack(spacing: 12) {
            HStack {
                VStack(alignment: .leading, spacing: 4) {
                    Text(runner.progressText)
                        .font(.headline)
                        .lineLimit(1)
                    Text("Running automated compatibility tests…")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
                Spacer()
                Button("Cancel", role: .destructive) {
                    runner.cancel()
                }
                .buttonStyle(.borderedProminent)
                .tint(.red)
                .controlSize(.small)
            }

            ProgressView(value: Double(runner.currentIndex), total: Double(max(1, runner.targets.count)))
                .tint(.accentColor)

            if let frame = runner.currentFrame {
                HStack(spacing: 12) {
                    Image(uiImage: UIImage(cgImage: frame))
                        .resizable()
                        .aspectRatio(contentMode: .fit)
                        .frame(height: 90)
                        .clipShape(RoundedRectangle(cornerRadius: 8))

                    VStack(alignment: .leading, spacing: 4) {
                        Text("\(runner.currentFPS) FPS")
                            .font(.callout.monospacedDigit().weight(.bold))
                            .foregroundStyle(runner.currentFPS >= 30 ? .green : .yellow)

                        if let lastLine = runner.lastLog {
                            Text(lastLine)
                                .font(.caption2.monospaced())
                                .foregroundStyle(.secondary)
                                .lineLimit(3)
                        }
                    }
                    Spacer()
                }
                .padding(8)
                .background(Color(uiColor: .tertiarySystemBackground), in: RoundedRectangle(cornerRadius: 10))
            }
        }
        .padding()
        .background(Color(uiColor: .secondarySystemBackground))
    }

    // MARK: - Empty State

    private var emptyStateView: some View {
        ContentUnavailableView {
            Label("Test All Games", systemImage: "checkmark.seal")
        } description: {
            Text("Run automated serial test passes across all installed games in your library. Generates picture checks, FPS reports, and crash logs.")
        } actions: {
            VStack(spacing: 12) {
                if BatchTestRunner.hasPendingSession() {
                    Button("Resume Interrupted Session") {
                        runner.resumePendingSession(allApps: installedApps)
                    }
                    .buttonStyle(.borderedProminent)
                    .tint(.orange)
                }

                Button("Configure & Start Batch Test") {
                    showOptionsSheet = true
                }
                .buttonStyle(.borderedProminent)
            }
        }
    }

    // MARK: - Results List

    private var resultsListView: some View {
        List {
            Section {
                summaryCard
            }

            Section("App Test Results (\(runner.results.count))") {
                ForEach(runner.results) { res in
                    BatchResultRow(result: res, runner: runner)
                }
            }
        }
    }

    private var summaryCard: some View {
        let total = runner.results.count
        let crashed = runner.results.filter(\.crashed).count
        let wrongSignal = runner.results.filter { !$0.crashed && $0.frame.signal != "ok" }.count
        let passed = total - crashed - wrongSignal

        return HStack(spacing: 16) {
            summaryStat(title: "Total", count: total, color: .primary)
            Divider()
            summaryStat(title: "Passed", count: passed, color: .green)
            Divider()
            summaryStat(title: "Crashed", count: crashed, color: .red)
            Divider()
            summaryStat(title: "Signal Err", count: wrongSignal, color: .orange)
        }
        .padding(.vertical, 8)
    }

    private func summaryStat(title: String, count: Int, color: Color) -> some View {
        VStack(spacing: 4) {
            Text("\(count)")
                .font(.title2.weight(.bold))
                .foregroundStyle(color)
            Text(title)
                .font(.caption2.weight(.medium))
                .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity)
    }

    // MARK: - Options Form Sheet

    private var optionsFormSheet: some View {
        NavigationStack {
            Form {
                Section("Test Duration") {
                    Stepper("Seconds per game: \(runner.options.secondsPerApp)s", value: Binding(get: {
                        runner.options.secondsPerApp
                    }, set: {
                        runner.options.secondsPerApp = $0
                    }), in: 5...60, step: 5)
                }

                Section("Renderer Selection") {
                    Picker("Renderer Mode", selection: Binding(get: {
                        runner.options.rendererMode
                    }, set: {
                        runner.options.rendererMode = $0
                    })) {
                        ForEach(BatchRendererMode.allCases) { mode in
                            Text(mode.displayName).tag(mode)
                        }
                    }
                    .pickerStyle(.segmented)
                }

                Section("Engine Options") {
                    Toggle("Continue past missing functions", isOn: Binding(get: {
                        runner.options.keepGoing
                    }, set: {
                        runner.options.keepGoing = $0
                    }))

                    Toggle("Reset app sandbox before test", isOn: Binding(get: {
                        runner.options.resetSandbox
                    }, set: {
                        runner.options.resetSandbox = $0
                    }))
                }

                Section {
                    Button("Start Batch Test") {
                        showOptionsSheet = false
                        runner.startNewBatch(apps: installedApps, options: runner.options)
                    }
                    .font(.headline)
                    .frame(maxWidth: .infinity, alignment: .center)
                    .foregroundStyle(.tint)
                }
            }
            .navigationTitle("Batch Test Options")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button("Done") { showOptionsSheet = false }
                }
            }
        }
    }

    private func prepareShareItems() {
        shareItems.removeAll()
        let jsonURL = BatchTestRunner.reportsDir.appending(path: "latest_batch_report.json")
        let mdURL = BatchTestRunner.reportsDir.appending(path: "latest_batch_report.md")

        if FileManager.default.fileExists(atPath: jsonURL.path) {
            shareItems.append(jsonURL)
        }
        if FileManager.default.fileExists(atPath: mdURL.path) {
            shareItems.append(mdURL)
        }
        if shareItems.isEmpty {
            shareItems.append(runner.buildMarkdownReport())
        }
    }
}

// MARK: - Batch Result Row

private struct BatchResultRow: View {
    let result: BatchAppResult
    let runner: BatchTestRunner
    @State private var isExpanded = false
    @Environment(\.openURL) private var openURL

    var statusBadge: (text: String, color: Color, icon: String) {
        if result.crashed {
            return ("Crashed", .red, "xmark.octagon.fill")
        } else if result.frame.signal != "ok" {
            return ("Output Error (\(result.frame.signal))", .orange, "exclamationmark.triangle.fill")
        } else {
            return ("Passed", .green, "checkmark.circle.fill")
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 12) {
                if let thumbPath = result.frame.thumbnailPath,
                   let uiImage = UIImage(contentsOfFile: BatchTestRunner.reportsDir.appending(path: thumbPath).path) {
                    Image(uiImage: uiImage)
                        .resizable()
                        .aspectRatio(contentMode: .fill)
                        .frame(width: 48, height: 48)
                        .clipShape(RoundedRectangle(cornerRadius: 8))
                } else {
                    Image(systemName: "gamecontroller")
                        .font(.title2)
                        .foregroundStyle(.secondary)
                        .frame(width: 48, height: 48)
                        .background(Color(uiColor: .tertiarySystemFill), in: RoundedRectangle(cornerRadius: 8))
                }

                VStack(alignment: .leading, spacing: 4) {
                    HStack {
                        Text(result.name)
                            .font(.headline)
                        Text("(\(result.mode.uppercased()))")
                            .font(.caption2.weight(.bold))
                            .padding(.horizontal, 5)
                            .padding(.vertical, 2)
                            .background(result.mode == "gpu" ? Color.purple.opacity(0.15) : Color.blue.opacity(0.15), in: Capsule())
                            .foregroundStyle(result.mode == "gpu" ? Color.purple : Color.blue)
                    }

                    Text(result.bundleId)
                        .font(.caption)
                        .foregroundStyle(.secondary)

                    let badge = statusBadge
                    HStack(spacing: 4) {
                        Image(systemName: badge.icon)
                            .font(.caption2)
                        Text(badge.text)
                            .font(.caption2.weight(.semibold))
                        Spacer()
                        Text("\(result.perf.fps) FPS · \(String(format: "%.1f", result.perf.frameTimeP95Ms))ms")
                            .font(.caption2.monospacedDigit())
                            .foregroundStyle(.secondary)
                    }
                    .foregroundStyle(badge.color)
                }
            }

            if isExpanded {
                VStack(alignment: .leading, spacing: 6) {
                    Divider()
                    Text("Summary: \(result.summary)")
                        .font(.caption.weight(.medium))

                    if !result.logs.isEmpty {
                        Text("Logs Excerpt:")
                            .font(.caption2.weight(.semibold))
                            .foregroundStyle(.secondary)
                        Text(result.logs)
                            .font(.system(size: 10, design: .monospaced))
                            .foregroundStyle(.secondary)
                            .padding(6)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .background(Color(uiColor: .secondarySystemBackground), in: RoundedRectangle(cornerRadius: 6))
                    }

                    if result.crashed || result.frame.signal != "ok" {
                        Button {
                            openURL(runner.consoleIssueURL(for: result))
                        } label: {
                            Label("Report Failure on GitHub", systemImage: "exclamationmark.bubble")
                                .font(.caption)
                        }
                        .padding(.top, 4)
                    }
                }
            }
        }
        .contentShape(Rectangle())
        .onTapGesture {
            withAnimation { isExpanded.toggle() }
        }
    }
}
