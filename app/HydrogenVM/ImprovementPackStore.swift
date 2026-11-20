import CryptoKit
import Foundation
import SwiftUI

/// Phase H2: On-device Ed25519-verified downloadable improvement pack store.
///
/// Verifies `hydrogenvm-improvement-pack` JSON envelopes signed by `tools/pack_sign.py`
/// using Apple's native CryptoKit `Curve25519.Signing.PublicKey`. Unsigned, malformed,
/// or tampered packs are strictly refused. A single toggle rolls back to built-in tables.
enum ImprovementPackStore {
    static let enabledKey = "hvm.improvementPacks.enabled"
    static let activeVersionKey = "hvm.improvementPacks.activeVersion"

    struct Envelope: Decodable {
        let format: String
        let algorithm: String
        let signature: String
        let payload: Payload
    }

    struct Payload: Codable {
        let schema_version: Int
        let pack_version: String
        let sections: [String: AnyCodableValue]
    }

    /// Minimal JSON value wrapper so canonical re-serialization matches `tools/pack_sign.py`.
    enum AnyCodableValue: Codable {
        case null
        case bool(Bool)
        case int(Int64)
        case double(Double)
        case string(String)
        case array([AnyCodableValue])
        case object([String: AnyCodableValue])

        init(from decoder: Decoder) throws {
            let c = try decoder.singleValueContainer()
            if c.decodeNil() { self = .null }
            else if let b = try? c.decode(Bool.self) { self = .bool(b) }
            else if let i = try? c.decode(Int64.self) { self = .int(i) }
            else if let d = try? c.decode(Double.self) { self = .double(d) }
            else if let s = try? c.decode(String.self) { self = .string(s) }
            else if let a = try? c.decode([AnyCodableValue].self) { self = .array(a) }
            else if let o = try? c.decode([String: AnyCodableValue].self) { self = .object(o) }
            else { throw DecodingError.dataCorruptedError(in: c, debugDescription: "Unsupported JSON value") }
        }

        func encode(to encoder: Encoder) throws {
            var c = encoder.singleValueContainer()
            switch self {
            case .null: try c.encodeNil()
            case .bool(let b): try c.encode(b)
            case .int(let i): try c.encode(i)
            case .double(let d): try c.encode(d)
            case .string(let s): try c.encode(s)
            case .array(let a): try c.encode(a)
            case .object(let o): try c.encode(o)
            }
        }
    }

    static var cacheURL: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? FileManager.default.temporaryDirectory
        let dir = base.appendingPathComponent("HydrogenVM/ImprovementPacks", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir.appendingPathComponent("active-pack.hvmp.json")
    }

    static var isEnabled: Bool {
        get { UserDefaults.standard.object(forKey: enabledKey) as? Bool ?? true }
        set { UserDefaults.standard.set(newValue, forKey: enabledKey) }
    }

    static var activePackVersion: String? {
        guard isEnabled, FileManager.default.fileExists(atPath: cacheURL.path) else { return nil }
        return UserDefaults.standard.string(forKey: activeVersionKey)
    }

    /// Verify a signed pack's Ed25519 signature against a 32-byte raw public key.
    static func verify(packData: Data, rawPublicKey: Data) -> Result<Payload, Error> {
        do {
            let env = try JSONDecoder().decode(Envelope.self, from: packData)
            guard env.format == "hydrogenvm-improvement-pack",
                  env.algorithm == "ed25519",
                  env.payload.schema_version == 1,
                  let sigData = Data(base64Encoded: env.signature),
                  sigData.count == 64 else {
                return .failure(NSError(domain: "ImprovementPack", code: 1,
                                        userInfo: [NSLocalizedDescriptionKey: "Malformed or unsigned improvement pack"]))
            }
            let encoder = JSONEncoder()
            encoder.outputFormatting = [.sortedKeys, .withoutEscapingSlashes]
            let canonical = try encoder.encode(env.payload)
            let pubKey = try Curve25519.Signing.PublicKey(rawRepresentation: rawPublicKey)
            guard pubKey.isValidSignature(sigData, for: canonical) else {
                return .failure(NSError(domain: "ImprovementPack", code: 2,
                                        userInfo: [NSLocalizedDescriptionKey: "Ed25519 signature verification failed"]))
            }
            return .success(env.payload)
        } catch {
            return .failure(error)
        }
    }

    /// Remove any cached improvement pack and revert to the app's built-in tables.
    static func rollbackToBuiltIn() {
        try? FileManager.default.removeItem(at: cacheURL)
        UserDefaults.standard.removeObject(forKey: activeVersionKey)
    }
}

/// Settings section shown in DiagnosticsView for managing signed improvement packs.
struct ImprovementPackSettingsSection: View {
    @AppStorage(ImprovementPackStore.enabledKey) private var enabled = true
    @State private var activeVersion: String? = ImprovementPackStore.activePackVersion
    @State private var statusMessage = ""

    var body: some View {
        Section("Improvement packs") {
            Toggle("Use signed improvement packs", isOn: $enabled)
            LabeledContent("Active pack", value: (enabled ? activeVersion : nil) ?? "Built-in tables")
            if activeVersion != nil {
                Button("Roll back to built-in tables", role: .destructive) {
                    ImprovementPackStore.rollbackToBuiltIn()
                    activeVersion = nil
                    statusMessage = "Reverted to built-in compatibility tables."
                }
            }
            if !statusMessage.isEmpty {
                Text(statusMessage).font(.footnote).foregroundStyle(.secondary)
            }
        }
    }
}
