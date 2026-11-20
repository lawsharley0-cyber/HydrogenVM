import SwiftUI

/// One game's page in the Gloss look: what it is, whether it runs, a Play button, its
/// per-game switches, and its files and data.
struct GameDetailView: View {
    let app: InstalledApp
    let play: () -> Void
    let browse: () -> Void
    let reset: () -> Void
    let delete: () -> Void

    @AppStorage private var allowInternet: Bool
    @AppStorage private var unlockFrameRate: Bool
    @AppStorage private var fastLaunchCache: Bool
    @AppStorage private var softwareRenderer: Bool
    @AppStorage private var virtualControllerEnabled: Bool
    @AppStorage(PlayerOverlay.settingKey) private var showOverlay = PlayerOverlay.settingDefault
    @State private var makingShortcut = false
    @State private var configuringController = false
    @State private var confirmingReset = false
    @State private var confirmingDelete = false

    init(app: InstalledApp, play: @escaping () -> Void, browse: @escaping () -> Void,
         reset: @escaping () -> Void, delete: @escaping () -> Void) {
        self.app = app
        self.play = play
        self.browse = browse
        self.reset = reset
        self.delete = delete
        let id = app.info.bundleIdentifier
        _allowInternet = AppStorage(wrappedValue: false, RunModel.internetKey(id))
        _unlockFrameRate = AppStorage(wrappedValue: false, RunModel.unlockFPSKey(id))
        _fastLaunchCache = AppStorage(wrappedValue: true, RunModel.fastLaunchCacheKey(id))
        _softwareRenderer = AppStorage(wrappedValue: RunModel.softwareRendering(for: id), RunModel.softwareRenderingKey(id))
        _virtualControllerEnabled = AppStorage(wrappedValue: false, VirtualControllerLayout.enabledKey(id))
    }

    private var details: String {
        app.info.isAndroid
            ? [app.info.version.isEmpty ? nil : app.info.version, "Android", "armeabi"].compactMap { $0 }.joined(separator: " · ")
            : [app.info.version.isEmpty ? nil : app.info.version,
               app.info.minimumOSVersion.isEmpty ? nil : "iOS \(app.info.minimumOSVersion)+",
               "armv7"].compactMap { $0 }.joined(separator: " · ")
    }

    var body: some View {
        ScrollView {
            VStack(spacing: 0) {
                HStack(spacing: 16) {
                    GlossIcon(path: app.info.iconPath, title: app.title, size: 76)
                    VStack(alignment: .leading, spacing: 3) {
                        Text(app.title)
                            .font(.system(size: 17, weight: .bold))
                            .foregroundStyle(Gloss.ink)
                            .shadow(color: Gloss.emboss, radius: 0, x: 0, y: 1)
                        Text(details).font(.system(size: 13)).foregroundStyle(Gloss.inkSoft)
                        Text(app.info.bundleIdentifier).font(.system(size: 12)).foregroundStyle(Gloss.inkSoft).lineLimit(1)
                        let s = AppRow.status(app.info)
                        Text(app.info.runnable ? s.text : (app.info.problem.isEmpty ? s.text : app.info.problem))
                            .font(.system(size: 13, weight: .semibold)).foregroundStyle(s.color)
                    }
                    Spacer(minLength: 0)
                }
                .padding(.horizontal, 20)
                .padding(.top, 20)
                .padding(.bottom, 12)

                Button(action: play) { Text("Play") }
                    .buttonStyle(GlossButtonStyle(kind: .green))
                    .disabled(!app.info.runnable)
                    .opacity(app.info.runnable ? 1 : 0.5)
                    .padding(.horizontal, 16)
                    .padding(.bottom, 6)

                GlossSectionLabel("Settings")
                GlossCard {
                    GlossToggleRow(title: "On-screen controller",
                                   hint: "Off by default. Shows a virtual thumbstick and buttons mapped to touch or tilt for this game.",
                                   isOn: $virtualControllerEnabled)
                        .onChange(of: virtualControllerEnabled) { _, isOn in
                            VirtualControllerLayout.setEnabled(isOn, bundleID: app.info.bundleIdentifier)
                        }
                    if virtualControllerEnabled {
                        GlossRule()
                        GlossLinkRow(title: "Controller layout & mapping", systemImage: "gamecontroller") {
                            configuringController = true
                        }
                    }
                    GlossRule()
                    GlossToggleRow(title: "Allow internet",
                                   hint: "Off: the game stays offline, with stand-in servers where HydrogenVM has them.",
                                   isOn: $allowInternet)
                    GlossRule()
                    GlossToggleRow(title: "Unlock frame rate",
                                   hint: "Experimental. Games capped at 30 fps run at 60; a game that counts frames may run fast.",
                                   isOn: $unlockFrameRate)
                    GlossRule()
                    GlossToggleRow(title: "Fast launch cache",
                                   hint: "Persists decoded ARM blocks on disk (hash-checked on load) to speed up repeat launches.",
                                   isOn: $fastLaunchCache)
                    GlossRule()
                    GlossToggleRow(title: "Software renderer",
                                   hint: "Draws on the CPU instead of the GPU. Slower; for games that draw wrongly.",
                                   isOn: $softwareRenderer)
                    GlossRule()
                    GlossToggleRow(title: "Show debug overlay",
                                   hint: "For all games. Frame time and log over the game.",
                                   isOn: $showOverlay)
                }
                Text("Changes apply the next time the game starts.")
                    .font(.system(size: 12)).foregroundStyle(Gloss.inkSoft)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 24).padding(.top, 6)

                GlossSectionLabel("Data")
                GlossCard {
                    GlossLinkRow(title: "Files", systemImage: "folder", action: browse)
                    GlossRule()
                    GlossLinkRow(title: "Add to Home Screen", systemImage: "plus.app") { makingShortcut = true }
                    GlossRule()
                    GlossLinkRow(title: "Reset game data", systemImage: "arrow.counterclockwise", tint: Gloss.warn) { confirmingReset = true }
                    GlossRule()
                    GlossLinkRow(title: "Delete game", systemImage: "trash", tint: Gloss.bad) { confirmingDelete = true }
                }
                .padding(.bottom, 24)
            }
        }
        .background(LinenBackground())
        .navigationTitle(app.title)
        .navigationBarTitleDisplayMode(.inline)
        .sheet(isPresented: $makingShortcut) { AddToHomeScreenSheet(app: app) }
        .sheet(isPresented: $configuringController) {
            VirtualControllerConfigSheet(bundleID: app.info.bundleIdentifier, gameTitle: app.title)
                .onDisappear {
                    virtualControllerEnabled = VirtualControllerLayout.load(bundleID: app.info.bundleIdentifier).enabled
                }
        }
        .confirmationDialog("Reset \(app.title)'s data?", isPresented: $confirmingReset, titleVisibility: .visible) {
            Button("Reset", role: .destructive, action: reset)
            Button("Cancel", role: .cancel) {}
        } message: { Text("Saves, settings and downloaded content are removed. The game itself stays.") }
        .confirmationDialog("Delete \(app.title)?", isPresented: $confirmingDelete, titleVisibility: .visible) {
            Button("Delete", role: .destructive, action: delete)
            Button("Cancel", role: .cancel) {}
        } message: { Text("The game and its saves are removed.") }
    }
}

/// A switch row with a one-line explanation under its title.
struct GlossToggleRow: View {
    let title: String
    let hint: String
    @Binding var isOn: Bool
    var body: some View {
        Toggle(isOn: $isOn) {
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.system(size: 15, weight: .semibold)).foregroundStyle(Gloss.ink)
                Text(hint).font(.system(size: 12)).foregroundStyle(Gloss.inkSoft).fixedSize(horizontal: false, vertical: true)
            }
        }
        .tint(Color(red: 0.247, green: 0.498, blue: 0.816))
        .padding(.horizontal, 14)
        .padding(.vertical, 10)
        .frame(minHeight: 60)
    }
}

/// A tappable row with an icon, a title and a chevron.
struct GlossLinkRow: View {
    let title: String
    let systemImage: String
    var tint: Color = Gloss.link
    let action: () -> Void
    var body: some View {
        Button(action: action) {
            HStack(spacing: 12) {
                Image(systemName: systemImage).font(.system(size: 16, weight: .semibold)).foregroundStyle(tint).frame(width: 24)
                Text(title).font(.system(size: 15, weight: .medium)).foregroundStyle(tint == Gloss.link ? Gloss.ink : tint)
                Spacer()
                Image(systemName: "chevron.right").font(.system(size: 13, weight: .semibold)).foregroundStyle(Gloss.chevron)
            }
            .padding(.horizontal, 14)
            .frame(minHeight: 48)
            .contentShape(.rect)
        }
        .buttonStyle(.plain)
    }
}
