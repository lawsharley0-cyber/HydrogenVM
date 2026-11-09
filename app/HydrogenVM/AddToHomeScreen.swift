import SafariServices
import SwiftUI
import UIKit

/// "Add to Home Screen" for one game, the way LiveContainer's "Create App Clip" does it.
///
/// HydrogenVM builds a web clip configuration profile with the game's own name and icon
/// whose URL is `hydrogenvm://run/<game-id>`, and opens it in Safari as a `data:` URL.
/// Safari offers to download it; the player installs it in Settings, and the game's icon
/// appears on the Home Screen. Tapping it opens HydrogenVM straight into the game.
/// iOS lets no app place a Home Screen icon by itself, so the Settings install is the
/// player's one step. No Shortcuts app involved.
struct AddToHomeScreenSheet: View {
    let app: InstalledApp
    @Environment(\.dismiss) private var dismiss
    @State private var profile: WebClipProfile?
    @State private var profileFile: URL?
    @State private var errorMessage: String?
    @State private var iconMissing = false
    @State private var showingSafari = false

    var body: some View {
        NavigationStack {
            List {
                Section {
                    HStack(spacing: 14) {
                        AppIcon(path: app.info.iconPath)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(app.title).font(.headline)
                            Text("Opens straight into the game").font(.caption).foregroundStyle(.secondary)
                        }
                    }
                }
                Section {
                    if let errorMessage {
                        Label(errorMessage, systemImage: "exclamationmark.triangle")
                            .foregroundStyle(.orange)
                    } else {
                        Button {
                            showingSafari = true
                        } label: {
                            Label("Add to Home Screen", systemImage: "plus.app.fill")
                                .font(.headline)
                        }
                        .disabled(profile?.dataURL == nil)
                        .accessibilityIdentifier("add-home-screen-clip")
                    }
                } footer: {
                    Text("1. Tap Allow when Safari asks to download a configuration profile.\n2. Open Settings, tap Profile Downloaded, then Install.\nThe icon appears on your Home Screen. To remove it, delete the icon or the profile in Settings → General → VPN & Device Management.")
                }
                if iconMissing {
                    Section {
                        Text("This game's icon could not be read, so iOS will draw a placeholder icon. The icon still opens the game.")
                            .font(.callout)
                            .foregroundStyle(.secondary)
                    }
                }
                if let profileFile {
                    Section {
                        ShareLink(item: profileFile) {
                            Label("Share the profile instead", systemImage: "square.and.arrow.up")
                        }
                        .accessibilityIdentifier("share-home-screen-profile")
                    } footer: {
                        Text("If Safari doesn't offer the download, save the profile to Files and open it there.")
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
            .task { makeProfile() }
            .fullScreenCover(isPresented: $showingSafari) {
                if let url = profile?.dataURL {
                    SafariView(url: url).ignoresSafeArea()
                }
            }
        }
    }

    private func makeProfile() {
        guard profile == nil, errorMessage == nil else { return }
        guard let link = GameLink.url(forGameID: app.info.bundleIdentifier) else {
            // A bundle id a link cannot carry: say so instead of making an icon that never opens.
            errorMessage = "This game's id can't be used in a link: \(app.info.bundleIdentifier)"
            return
        }
        let icon = Self.iconPNG(app.info.iconPath)
        iconMissing = icon == nil
        let made = WebClipProfile(gameID: app.info.bundleIdentifier, title: app.title, link: link, iconPNG: icon)
        profile = made
        // The same profile as a file, for the share fallback. One folder per game, so two
        // games with the same name never overwrite each other.
        let folder = URL.temporaryDirectory
            .appending(path: "Home Screen", directoryHint: .isDirectory)
            .appending(path: app.info.bundleIdentifier, directoryHint: .isDirectory)
        let file = folder.appending(path: made.filename)
        do {
            try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
            try Data(made.payload().utf8).write(to: file, options: .atomic)
            profileFile = file
        } catch {
            profileFile = nil   // Safari still works without the file
        }
    }

    /// The game's own icon as PNG. `AppIcon.load` handles ordinary PNGs and the
    /// Xcode-crushed (CgBI) icons old apps ship.
    private static func iconPNG(_ path: String?) -> Data? {
        guard let path, let image = AppIcon.load(path) else { return nil }
        return image.pngData()
    }
}

/// Safari inside the app, as LiveContainer presents its web clip profile.
private struct SafariView: UIViewControllerRepresentable {
    let url: URL
    func makeUIViewController(context: Context) -> SFSafariViewController { SFSafariViewController(url: url) }
    func updateUIViewController(_ controller: SFSafariViewController, context: Context) {}
}
