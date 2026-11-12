import SwiftUI

/// Everyday preferences stay separate from troubleshooting controls.
struct LibrarySettingsView: View {
    @Binding var keepGoing: Bool
    let installedApps: [InstalledApp]
    @State private var showingBatchTest = false
    @Environment(\.dismiss) private var dismiss
    @AppStorage("keepIpaForReinstall") private var keepIPA = true
    @AppStorage(PlayerSettings.scaleModeKey) private var scaleMode = PlayerScaleMode.fit.rawValue
    @AppStorage(PlayerOverlay.settingKey) private var debugOverlay = PlayerOverlay.settingDefault
    @AppStorage("player.showControlsButton") private var controlsButton = true
    @AppStorage("hvm.zombies") private var zombies = false

    var body: some View {
        NavigationStack {
            Form {
                Section("Playing") {
                    Picker("Default display mode", selection: $scaleMode) {
                        ForEach(PlayerScaleMode.allCases) { mode in
                            Text(mode.rawValue).tag(mode.rawValue)
                        }
                    }
                    .pickerStyle(.menu)
                }
                Section {
                    Toggle("Show player controls button", isOn: $controlsButton)
                    Text("A button in the top corner opens pause, volume, display options and quit. Turn this on for access without a multi-finger gesture.")
                        .font(.footnote).foregroundStyle(.secondary)
                    Text("If the button is hidden, hold three fingers on the game screen to open controls.")
                        .font(.footnote).foregroundStyle(.secondary)
                } header: {
                    Text("Accessibility & controls")
                }
                Section {
                    Toggle("Keep IPA for reinstall", isOn: $keepIPA)
                } header: {
                    Text("Storage")
                } footer: {
                    Text("Keeps a copy of future imports so you can reinstall without choosing the file again. Existing copies are kept when this is turned off.")
                }
                Section {
                    Toggle("Show performance overlay", isOn: $debugOverlay)
                    Toggle("Continue past missing functions", isOn: $keepGoing)
                    Toggle("Track freed objects (zombies)", isOn: $zombies)
                    Button("Test all games", systemImage: "play.grid.2x2") { showingBatchTest = true }
                } header: {
                    Text("Advanced troubleshooting")
                } footer: {
                    Text("Continuing past missing functions can produce incomplete behavior. Tracking freed objects is a debugging tool. Leave both off for everyday use.")
                }
            }
            .navigationTitle("Settings")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .sheet(isPresented: $showingBatchTest) {
                BatchTestView(installedApps: installedApps)
            }
        }
    }
}
