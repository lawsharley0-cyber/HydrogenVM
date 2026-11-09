import Network
import SafariServices
import SwiftUI
import UIKit

/// "Add to Home Screen" for one game, the way LiveContainer's "Create App Clip" does it.
///
/// HydrogenVM builds a web clip configuration profile with the game's own name and icon
/// whose URL is `hydrogenvm://run/<game-id>`. LiveContainer opens its profile as a `data:`
/// URL; SFSafariViewController only accepts http(s), so HydrogenVM serves the profile once
/// from a loopback address (`ProfileServer`) and opens that. Safari offers to download it; the player installs it in Settings, and the game's icon
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
    @State private var server: ProfileServer?
    @State private var serverURL: URL?

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
                            openInSafari()
                        } label: {
                            Label("Add to Home Screen", systemImage: "plus.app.fill")
                                .font(.headline)
                        }
                        .disabled(profile == nil)
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
                if let serverURL {
                    SafariView(url: serverURL).ignoresSafeArea()
                }
            }
            .onDisappear { server?.stop(); server = nil }
        }
    }

    /// Serve the profile on 127.0.0.1 and show it in Safari. If the server cannot start,
    /// the share button below still hands the profile over.
    private func openInSafari() {
        guard let profile else { return }
        // One server per sheet: a second tap reuses it, or waits while it starts.
        if server != nil {
            if serverURL != nil { showingSafari = true }
            return
        }
        let made = ProfileServer(body: Data(profile.payload().utf8), filename: profile.filename)
        made.start { url in
            guard let url else {
                errorMessage = "Couldn't hand the profile to Safari. Use Share the profile instead."
                return
            }
            serverURL = url
            showingSafari = true
        }
        server = made
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

/// Serves one configuration profile over HTTP on 127.0.0.1, so Safari can download it.
/// Loopback only: nothing off the phone can reach it. Stops when the sheet closes.
final class ProfileServer: @unchecked Sendable {
    private let body: Data
    private let filename: String
    private var listener: NWListener?
    private let queue = DispatchQueue(label: "HydrogenVM.ProfileServer")

    init(body: Data, filename: String) {
        self.body = body
        self.filename = filename
    }

    /// Calls `ready` with the profile's URL, or nil if the listener could not start.
    func start(ready: @escaping @MainActor (URL?) -> Void) {
        let parameters = NWParameters.tcp
        parameters.requiredLocalEndpoint = NWEndpoint.hostPort(host: .ipv4(.loopback), port: .any)
        guard let listener = try? NWListener(using: parameters) else {
            Task { @MainActor in ready(nil) }
            return
        }
        self.listener = listener
        let name = filename.addingPercentEncoding(withAllowedCharacters: .urlPathAllowed) ?? "Game.mobileconfig"
        let once = Once()   // handlers run on `queue`, one at a time
        listener.stateUpdateHandler = { [weak listener] state in
            switch state {
            case .ready:
                guard let port = listener?.port?.rawValue, once.claim() else { return }
                let url = URL(string: "http://127.0.0.1:\(port)/\(name)")
                Task { @MainActor in ready(url) }
            case .failed, .cancelled:
                if once.claim() { Task { @MainActor in ready(nil) } }
            default:
                break
            }
        }
        listener.newConnectionHandler = { [weak self] connection in self?.serve(connection) }
        listener.start(queue: queue)
    }

    func stop() {
        listener?.cancel()
        listener = nil
    }

    deinit {
        listener?.cancel()   // never leave a listener running without its server
    }

    /// Reads the request (any path), answers with the profile and closes.
    private func serve(_ connection: NWConnection) {
        connection.start(queue: queue)
        connection.receive(minimumIncompleteLength: 1, maximumLength: 16 * 1024) { [body] _, _, _, _ in
            var response = Data(("HTTP/1.1 200 OK\r\n" +
                                 "Content-Type: application/x-apple-aspen-config\r\n" +
                                 "Content-Length: \(body.count)\r\n" +
                                 "Connection: close\r\n\r\n").utf8)
            response.append(body)
            connection.send(content: response, completion: .contentProcessed { _ in connection.cancel() })
        }
    }
}

/// A flag set once; only touched from the server's serial queue.
// swiftlint:disable:next required_deinit
private final class Once: @unchecked Sendable {
    private var done = false
    func claim() -> Bool {
        if done { return false }
        done = true
        return true
    }
}
