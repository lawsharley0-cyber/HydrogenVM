import Intents
import IntentsUI
import SwiftUI
import UIKit

/// "Add to Home Screen" for one game, without a configuration profile.
///
/// Step 1 (one tap here): the system Add to Siri sheet saves a "Play <game>" shortcut
/// to the Shortcuts library. Step 2 (in Shortcuts): touch and hold it, then Add to
/// Home Screen. iOS does not let an app place a Home Screen icon by itself, so the
/// second step is the player's; the shortcut opens HydrogenVM straight into the game
/// (`GameShortcut` -> `LaunchRouter`).
struct AddToHomeScreenSheet: View {
    let app: InstalledApp
    @Environment(\.dismiss) private var dismiss
    @Environment(\.openURL) private var openURL
    @State private var presentingSiri = false
    @State private var added = false
    @State private var errorMessage: String?

    var body: some View {
        NavigationStack {
            List {
                Section {
                    Text("Adds “\(GameShortcut.title(forGameNamed: app.title))” to Shortcuts. From there you can put it on your Home Screen; tapping it opens HydrogenVM straight into the game. Nothing to install.")
                }
                Section {
                    if let errorMessage {
                        Label(errorMessage, systemImage: "exclamationmark.triangle")
                            .foregroundStyle(.orange)
                    } else {
                        Button {
                            presentingSiri = true
                        } label: {
                            Label(added ? "Added to Shortcuts" : "Add to Shortcuts", systemImage: added ? "checkmark.circle.fill" : "plus.circle")
                        }
                        .disabled(added)
                        .accessibilityIdentifier("add-game-shortcut")
                    }
                }
                Section("Then, on the Home Screen") {
                    Text("Open Shortcuts, touch and hold “\(GameShortcut.title(forGameNamed: app.title))”, choose Share or Details, then Add to Home Screen.")
                        .font(.callout)
                    Button {
                        if let url = URL(string: "shortcuts://") { openURL(url) }
                    } label: {
                        Label("Open Shortcuts", systemImage: "arrow.up.forward.app")
                    }
                    .accessibilityIdentifier("open-shortcuts-app")
                }
            }
            .navigationTitle("Add to Home Screen")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .confirmationAction) {
                    Button("Done") { dismiss() }
                }
            }
            .onAppear {
                if GameShortcut.userInfo(forGameID: app.info.bundleIdentifier) == nil {
                    errorMessage = "This game's id can't be used in a shortcut: \(app.info.bundleIdentifier)"
                }
            }
            .sheet(isPresented: $presentingSiri) {
                AddVoiceShortcut(activity: Self.activity(for: app)) { saved in
                    presentingSiri = false
                    if saved { added = true }
                }
                .ignoresSafeArea()
            }
        }
    }

    /// The activity the shortcut runs. Eligible for prediction so it also shows up as a
    /// Siri suggestion for games the player opens often.
    static func activity(for app: InstalledApp) -> NSUserActivity {
        let activity = NSUserActivity(activityType: GameShortcut.activityType)
        activity.title = GameShortcut.title(forGameNamed: app.title)
        activity.userInfo = GameShortcut.userInfo(forGameID: app.info.bundleIdentifier)
        activity.requiredUserInfoKeys = [GameShortcut.gameKey]
        activity.persistentIdentifier = app.info.bundleIdentifier
        activity.isEligibleForPrediction = true
        activity.isEligibleForSearch = true
        activity.suggestedInvocationPhrase = GameShortcut.title(forGameNamed: app.title)
        return activity
    }
}

/// The system Add to Siri sheet for one activity.
private struct AddVoiceShortcut: UIViewControllerRepresentable {
    let activity: NSUserActivity
    let done: @MainActor (Bool) -> Void

    func makeCoordinator() -> Coordinator { Coordinator(done: done) }

    func makeUIViewController(context: Context) -> INUIAddVoiceShortcutViewController {
        let controller = INUIAddVoiceShortcutViewController(shortcut: INShortcut(userActivity: activity))
        controller.delegate = context.coordinator
        return controller
    }

    func updateUIViewController(_ controller: INUIAddVoiceShortcutViewController, context: Context) {}

    // IntentsUI calls its delegate on the main thread.
    @MainActor
    final class Coordinator: NSObject, @preconcurrency INUIAddVoiceShortcutViewControllerDelegate {
        let done: @MainActor (Bool) -> Void
        init(done: @escaping @MainActor (Bool) -> Void) { self.done = done }

        func addVoiceShortcutViewController(_ controller: INUIAddVoiceShortcutViewController,
                                            didFinishWith voiceShortcut: INVoiceShortcut?, error: Error?) {
            done(voiceShortcut != nil && error == nil)
        }

        func addVoiceShortcutViewControllerDidCancel(_ controller: INUIAddVoiceShortcutViewController) {
            done(false)
        }
    }
}
