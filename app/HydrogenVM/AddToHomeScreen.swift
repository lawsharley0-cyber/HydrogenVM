import SwiftUI
import UIKit

/// "Add to Home Screen" for one game: writes a Web Clip profile and hands it to the
/// system share sheet. iOS only adds the icon after the player approves the profile in
/// Settings; an app cannot install a profile or create a Home Screen icon by itself.
struct AddToHomeScreenSheet: View {
    let app: InstalledApp
    @Environment(\.dismiss) private var dismiss
    @State private var profileURL: URL?
    @State private var errorMessage: String?
    @State private var iconMissing = false

    var body: some View {
        NavigationStack {
            List {
                Section {
                    Text("This adds “\(app.title)” to the Home Screen with its own icon. Tapping the icon opens HydrogenVM straight into the game.")
                }
                Section("Install") {
                    Text("1. Tap Share, choose Save to Files.\n2. In Files, tap the .mobileconfig file.\n3. Allow the download, then tap Install in Settings.")
                        .font(.callout)
                    if let profileURL {
                        ShareLink(item: profileURL) {
                            Label("Share the profile", systemImage: "square.and.arrow.up")
                        }
                        .accessibilityIdentifier("share-home-screen-profile")
                        Text(profileURL.lastPathComponent)
                            .font(.caption.monospaced())
                            .foregroundStyle(.secondary)
                    } else if let errorMessage {
                        Label(errorMessage, systemImage: "exclamationmark.triangle")
                            .foregroundStyle(.orange)
                    } else {
                        ProgressView()
                    }
                }
                if iconMissing {
                    Section {
                        Text("This game's icon could not be read, so iOS will draw its own placeholder icon. The link still opens the game.")
                            .font(.callout)
                            .foregroundStyle(.secondary)
                    }
                }
            }
            .navigationTitle("Add to Home Screen")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .task { writeProfile() }
        }
    }

    private func writeProfile() {
        guard profileURL == nil, errorMessage == nil else { return }
        guard let link = GameLink.url(forGameID: app.info.bundleIdentifier) else {
            // A bundle id with characters a link cannot carry (empty, too long, unusual
            // spelling): say so instead of writing a profile that would never open.
            errorMessage = "This game's id can't be used in a link: \(app.info.bundleIdentifier)"
            return
        }
        let icon = Self.iconPNG(app.info.iconPath)
        iconMissing = icon == nil
        let profile = WebClipProfile(gameID: app.info.bundleIdentifier, title: app.title,
                                    link: link, iconPNG: icon)
        // One folder per game, so two games called the same thing never overwrite each other.
        let folder = URL.documentsDirectory
            .appending(path: "Home Screen", directoryHint: .isDirectory)
            .appending(path: app.info.bundleIdentifier, directoryHint: .isDirectory)
        let file = folder.appending(path: profile.filename)
        do {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            try Data(profile.payload().utf8).write(to: file, options: .atomic)
            profileURL = file
        } catch {
            errorMessage = "Could not write the profile: \(error.localizedDescription)"
        }
    }

    /// The game's own icon as PNG. `AppIcon.load` handles both ordinary PNGs and the
    /// Xcode-crushed (CgBI) icons old apps ship.
    private static func iconPNG(_ path: String?) -> Data? {
        guard let path, let image = AppIcon.load(path) else { return nil }
        return image.pngData()
    }
}
