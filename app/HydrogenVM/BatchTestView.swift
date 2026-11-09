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
    /// "Test every game" was turned off and no game is picked yet.
    @State private var choosingGames = false
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
            .onChange(of: showOptionsSheet) { _, shown in
                // Once per presentation (not when returning from the game picker): show the
                // player's remembered pick, not a retest's list, minus games removed since.
                guard shown else { return }
                let installed = Set(runnableApps.map(\.info.bundleIdentifier))
                runner.options.onlyGames = BatchTestRunner.rememberedGameChoice().filter { installed.contains($0) }
                choosingGames = !runner.options.onlyGames.isEmpty
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
            Text("Run automated test passes across the games in your library, or only the ones you pick. Generates picture checks, FPS reports, and crash logs.")
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

            if runner.status != .running, !runner.gamesWithProblems.isEmpty {
                Section {
                    Button {
                        runner.retest(runner.gamesWithProblems, apps: installedApps)
                    } label: {
                        Label("Retest the \(runner.gamesWithProblems.count) games with problems", systemImage: "arrow.clockwise")
                    }
                    .accessibilityIdentifier("batch-retest-problems")
                } footer: {
                    Text("Runs only the games that crashed, showed no picture or failed a check, with the same options.")
                }
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

        return VStack(spacing: 6) {
            HStack(spacing: 16) {
                summaryStat(title: "Total", count: total, color: .primary)
                Divider()
                summaryStat(title: "Passed", count: passed, color: .green)
                Divider()
                summaryStat(title: "Crashed", count: crashed, color: .red)
                Divider()
                summaryStat(title: "Signal Err", count: wrongSignal, color: .orange)
            }
            audioSummary
        }
        .padding(.vertical, 8)
    }

    /// How many games played music / sound effects, and how many failed a check.
    private var audioSummary: some View {
        let music = runner.results.filter { ($0.diagnostics?.musicFrames ?? 0) > 0 }.count
        let effects = runner.results.filter { ($0.diagnostics?.effectFrames ?? 0) > 0 }.count
        let failed = runner.results.filter { $0.worstCheck == .fail }.count
        return HStack(spacing: 12) {
            Label("\(music) music", systemImage: "music.note")
            Label("\(effects) sound fx", systemImage: "speaker.wave.2.fill")
            Label("\(failed) failed a check", systemImage: "checklist")
        }
        .font(.caption2)
        .foregroundStyle(.secondary)
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

    private var runnableApps: [InstalledApp] { installedApps.filter(\.info.runnable) }

    /// "All 38 games" or "3 of 38 games".
    private var gameChoiceSummary: String {
        let total = runnableApps.count
        let chosen = runner.options.games(from: installedApps).count
        return runner.options.onlyGames.isEmpty ? "All \(total) games" : "\(chosen) of \(total) games"
    }

    private var optionsFormSheet: some View {
        NavigationStack {
            Form {
                Section {
                    Toggle("Test every game", isOn: Binding(get: {
                        runner.options.onlyGames.isEmpty && !choosingGames
                    }, set: { every in
                        // Off: the player picks games below. On: forget the pick.
                        if every { runner.options.onlyGames = [] }
                        choosingGames = !every
                    }))
                    .accessibilityIdentifier("batch-test-every-game")
                    if choosingGames {
                        NavigationLink {
                            BatchGamePicker(apps: runnableApps, chosen: Binding(get: {
                                Set(runner.options.onlyGames)
                            }, set: { ids in
                                // Library order, so runs and reports list games the same way.
                                runner.options.onlyGames = runnableApps.map(\.info.bundleIdentifier).filter { ids.contains($0) }
                            }))
                        } label: {
                            LabeledContent("Games to test", value: gameChoiceSummary)
                        }
                        .accessibilityIdentifier("batch-choose-games")
                    }
                } header: {
                    Text("Games")
                } footer: {
                    Text("Pick a few games to test just those. The choice is kept for the next run.")
                }

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
                    Toggle("Trace graphics calls (GPU)", isOn: Binding(get: {
                        runner.options.glTrace
                    }, set: {
                        runner.options.glTrace = $0
                    }))
                    Toggle("Continue past missing functions", isOn: Binding(get: {
                        runner.options.keepGoing
                    }, set: {
                        runner.options.keepGoing = $0
                    }))

                    Toggle("Start each game with empty data", isOn: Binding(get: {
                        runner.options.resetSandbox
                    }, set: {
                        runner.options.resetSandbox = $0
                    }))

                    Toggle("Explore games automatically", isOn: Binding(get: {
                        runner.options.exploreEnabled
                    }, set: {
                        runner.options.exploreEnabled = $0
                    }))

                    if runner.options.exploreEnabled {
                        Picker("Explore Duration", selection: Binding(get: {
                            runner.options.exploreTime
                        }, set: {
                            runner.options.exploreTime = $0
                        })) {
                            Text("30 s").tag(30)
                            Text("60 s").tag(60)
                            Text("120 s").tag(120)
                        }
                    }

                    Toggle("Relaunch each game on its saved data", isOn: Binding(get: {
                        runner.options.relaunchEnabled
                    }, set: {
                        runner.options.relaunchEnabled = $0
                    }))
                }

                Section {
                    Label("Launch, picture, graphics calls (GPU, with tracing), game view on screen, audio output, music, sound effects, touch, background & resume, clean close, relaunch, missing APIs, handled faults and speed.", systemImage: "checklist")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                } header: {
                    Text("Checks run on every game")
                }

                Section {
                    Button(runner.options.onlyGames.isEmpty ? "Start Batch Test" : "Test \(gameChoiceSummary)") {
                        showOptionsSheet = false
                        choosingGames = false
                        BatchTestRunner.rememberGameChoice(runner.options.onlyGames)   // the player's pick, for next time
                        runner.startNewBatch(apps: installedApps, options: runner.options)
                    }
                    .disabled(choosingGames && runner.options.onlyGames.isEmpty)
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
        // The whole run as one folder (reports, screenshots, logs): save it to the
        // desktop's test_ipa folder so the desktop bench can read and compare it.
        do {
            shareItems.append(try runner.exportBundle())
            return
        } catch {
            // Say so rather than silently sharing less: the folder is what the desktop compares.
            shareItems.append("HydrogenVM could not export the test folder (\(error.localizedDescription)); sharing the report only.")
        }
        let jsonURL = BatchTestRunner.reportsDir.appending(path: "latest_batch_report.json")
        let mdURL = BatchTestRunner.reportsDir.appending(path: "latest_batch_report.md")

        if FileManager.default.fileExists(atPath: jsonURL.path) {
            shareItems.append(jsonURL)
        }
        if FileManager.default.fileExists(atPath: mdURL.path) {
            shareItems.append(mdURL)
        }
        if !shareItems.contains(where: { $0 is URL }) {
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
        } else if let checks = result.checks {
            let fails = checks.filter { $0.status == .fail }.count
            let warns = checks.filter { $0.status == .warn }.count
            if fails > 0 { return ("\(fails) check\(fails == 1 ? "" : "s") failed", .red, "xmark.circle.fill") }
            if warns > 0 { return ("Passed · \(warns) warning\(warns == 1 ? "" : "s")", .orange, "checkmark.circle.badge.questionmark") }
            return ("Passed all checks", .green, "checkmark.circle.fill")
        } else {
            return ("Passed", .green, "checkmark.circle.fill")
        }
    }

    static func color(_ st: BatchCheck.Status) -> Color {
        switch st {
        case .pass: return .green
        case .warn: return .orange
        case .fail: return .red
        case .skip: return .secondary
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

                    if let checks = result.checks {
                        // One dot per check, so a glance shows which ones failed.
                        HStack(spacing: 3) {
                            ForEach(checks) { c in
                                Image(systemName: c.icon)
                                    .font(.system(size: 9))
                                    .foregroundStyle(Self.color(c.status))
                                    .accessibilityLabel("\(c.name): \(c.status.rawValue)")
                            }
                        }
                    }
                }
            }

            if isExpanded {
                VStack(alignment: .leading, spacing: 6) {
                    Divider()
                    Text("Summary: \(result.summary)")
                        .font(.caption.weight(.medium))

                    if let checks = result.checks {
                        VStack(alignment: .leading, spacing: 4) {
                            ForEach(checks) { c in
                                HStack(alignment: .firstTextBaseline, spacing: 6) {
                                    Image(systemName: c.icon)
                                        .font(.caption2)
                                        .foregroundStyle(Self.color(c.status))
                                    Text(c.name)
                                        .font(.caption.weight(.semibold))
                                    Text(c.detail)
                                        .font(.caption)
                                        .foregroundStyle(.secondary)
                                        .fixedSize(horizontal: false, vertical: true)
                                }
                            }
                        }
                        .padding(.vertical, 2)
                    }

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

                    if result.crashed || result.frame.signal != "ok" || result.worstCheck == .fail {
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

// MARK: - Game picker

/// Choose which games a batch run covers. Search, select all, clear.
private struct BatchGamePicker: View {
    let apps: [InstalledApp]
    @Binding var chosen: Set<String>
    @State private var search = ""

    private var shown: [InstalledApp] {
        let query = search.trimmingCharacters(in: .whitespaces)
        guard !query.isEmpty else { return apps }
        return apps.filter { $0.title.localizedCaseInsensitiveContains(query) || $0.info.bundleIdentifier.localizedCaseInsensitiveContains(query) }
    }

    var body: some View {
        List {
            Section {
                ForEach(shown) { app in
                    let id = app.info.bundleIdentifier
                    Button {
                        if chosen.contains(id) { chosen.remove(id) } else { chosen.insert(id) }
                    } label: {
                        HStack {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(app.title).foregroundStyle(.primary)
                                Text(app.info.version.isEmpty ? id : "\(app.info.version) · \(id)")
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                            }
                            Spacer()
                            Image(systemName: chosen.contains(id) ? "checkmark.circle.fill" : "circle")
                                .foregroundStyle(chosen.contains(id) ? Color.accentColor : .secondary)
                                .imageScale(.large)
                        }
                    }
                    .accessibilityIdentifier("batch-pick-\(id)")
                }
            } footer: {
                Text("\(chosen.count) selected")
            }
        }
        .searchable(text: $search, prompt: "Find a game")
        .navigationTitle("Games to Test")
        .navigationBarTitleDisplayMode(.inline)
        .toolbar {
            ToolbarItem(placement: .topBarTrailing) {
                Menu {
                    Button("Select All Shown") { chosen.formUnion(shown.map(\.info.bundleIdentifier)) }
                    Button("Clear Selection", role: .destructive) { chosen.removeAll() }
                } label: {
                    Image(systemName: "checklist")
                }
            }
        }
    }
}
