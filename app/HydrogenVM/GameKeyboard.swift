import SwiftUI
import UIKit

/// Maps the traits a guest text field asked for (raw UIKit values) onto SwiftUI's input modifiers.
enum GuestKeyboard {
    static func keyboardType(_ raw: Int) -> UIKeyboardType { UIKeyboardType(rawValue: raw) ?? .default }
    static func autocapitalization(_ raw: Int) -> TextInputAutocapitalization? {
        switch raw {   // UITextAutocapitalizationType
        case 0: return .never
        case 1: return .words
        case 3: return .characters
        default: return .sentences
        }
    }
    static func autocorrectionDisabled(_ raw: Int) -> Bool { raw == 1 }   // UITextAutocorrectionTypeNo
    static func submitLabel(_ raw: Int) -> SubmitLabel {
        switch raw {   // UIReturnKeyType
        case 1: return .go
        case 2, 6, 8: return .search
        case 3: return .join
        case 4: return .next
        case 5: return .route
        case 7: return .send
        case 9: return .done
        case 11: return .continue
        default: return .return
        }
    }
}

/// The native keyboard for a focused guest UITextField / UITextView (#303). The game draws the text itself; this
/// bar is where the player types, and every edit is sent to the guest, which runs its own delegate checks and may
/// send a different text back (a refused edit), in which case the bar follows the guest.
struct GameKeyboardBar: View {
    let request: HVMTextInputRequest
    /// The new full text and the edit's sequence number (the guest echoes it back as `ackSeq`).
    let onText: (String, UInt64) -> Void
    let onReturn: () -> Void
    let onDismiss: () -> Void
    @State private var text = ""
    @State private var sent: UInt64 = 0
    @State private var returning = false
    @FocusState private var focused: Bool

    var body: some View {
        HStack(spacing: 8) {
            field
                .focused($focused)
                .keyboardType(GuestKeyboard.keyboardType(request.keyboardType))
                .textInputAutocapitalization(GuestKeyboard.autocapitalization(request.autocapitalizationType))
                .autocorrectionDisabled(GuestKeyboard.autocorrectionDisabled(request.autocorrectionType) || request.secure)
                .submitLabel(GuestKeyboard.submitLabel(request.returnKeyType))
                .onSubmit {
                    // Return goes to the guest, which decides whether the keyboard stays (moving to the next field,
                    // or resigning, which removes this bar). SwiftUI drops focus on submit, so take it back.
                    returning = true
                    onReturn()
                    focused = true
                    Task { @MainActor in returning = false }
                }
                .textFieldStyle(.roundedBorder)
                .accessibilityIdentifier("game-keyboard-field")
            Button("Done") { onDismiss() }
                .accessibilityIdentifier("game-keyboard-done")
        }
        .padding(10)
        .background(.regularMaterial)
        .onAppear { text = request.text; focused = true }
        .onChange(of: ObjectIdentifier(request)) { _, _ in
            // A correction from the guest (a refused edit, or text it rewrote) applies only if the player has not
            // typed anything newer since: otherwise it would overwrite that edit with stale text.
            if request.ackSeq >= sent, text != request.text { text = request.text }
        }
        .onChange(of: text) { _, new in
            sent += 1
            onText(new, sent)
        }
        .onChange(of: focused) { _, now in
            // The player tapped away (not Return, which refocuses above): close the guest field's editing.
            if !now && !returning { onDismiss() }
        }
    }

    @ViewBuilder private var field: some View {
        if request.secure {
            SecureField(request.placeholder, text: $text)
        } else if request.multiline {
            TextField(request.placeholder, text: $text, axis: .vertical).lineLimit(1...5)
        } else {
            TextField(request.placeholder, text: $text)
        }
    }
}
