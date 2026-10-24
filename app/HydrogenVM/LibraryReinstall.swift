import Foundation
import CryptoKit
import zlib

extension String {
    var sha256: String {
        let data = Data(self.utf8)
        let hash = SHA256.hash(data: data)
        return hash.compactMap { String(format: "%02x", $0) }.joined()
    }
}

extension URL {
    var sha256: String? {
        guard let data = try? Data(contentsOf: self, options: .mappedIfSafe) else { return nil }
        let hash = SHA256.hash(data: data)
        return hash.compactMap { String(format: "%02x", $0) }.joined()
    }
}

struct ZipEntry {
    let name: String
    let crc32: UInt32
    let size: UInt32
    var isDirectory: Bool { name.hasSuffix("/") }
}

func readZipEntries(from url: URL) throws -> [ZipEntry] {
    let data = try Data(contentsOf: url, options: .mappedIfSafe)
    guard data.count >= 22 else { return [] }

    // Find End of Central Directory (EOCD)
    var eocdOffset: Int? = nil
    // Scan backwards. EOCD is at least 22 bytes, max comment length is 65535, so at most ~65557 bytes from the end.
    let scanLimit = max(0, data.count - 65557)
    for i in (scanLimit...(data.count - 22)).reversed() {
        if data[i] == 0x50 && data[i+1] == 0x4b && data[i+2] == 0x05 && data[i+3] == 0x06 {
            eocdOffset = i
            break
        }
    }
    guard let eocdOffset = eocdOffset else { return [] }

    let cdOffset = Int(data[eocdOffset+16..<eocdOffset+20].withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) })
    let cdRecords = Int(data[eocdOffset+10..<eocdOffset+12].withUnsafeBytes { $0.loadUnaligned(as: UInt16.self) })

    var entries = [ZipEntry]()
    var offset = cdOffset

    for _ in 0..<cdRecords {
        guard offset + 46 <= eocdOffset else { break }
        guard data[offset] == 0x50 && data[offset+1] == 0x4b && data[offset+2] == 0x01 && data[offset+3] == 0x02 else { break }

        let crc = data[offset+16..<offset+20].withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) }
        let size = data[offset+24..<offset+28].withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) }
        let nameLen = Int(data[offset+28..<offset+30].withUnsafeBytes { $0.loadUnaligned(as: UInt16.self) })
        let extraLen = Int(data[offset+30..<offset+32].withUnsafeBytes { $0.loadUnaligned(as: UInt16.self) })
        let commentLen = Int(data[offset+32..<offset+34].withUnsafeBytes { $0.loadUnaligned(as: UInt16.self) })

        guard offset + 46 + nameLen <= eocdOffset else { break }

        let nameData = data[offset+46..<offset+46+nameLen]
        guard let name = String(data: nameData, encoding: .utf8) ?? String(data: nameData, encoding: .macOSRoman) else { break }

        entries.append(ZipEntry(name: name, crc32: crc, size: size))

        offset += 46 + nameLen + extraLen + commentLen
    }
    return entries
}

func fileCRC32(url: URL) -> UInt32? {
    guard let data = try? Data(contentsOf: url, options: .mappedIfSafe) else { return nil }
    var crc = crc32(0, nil, 0)
    data.withUnsafeBytes { buffer in
        if let baseAddress = buffer.baseAddress {
            crc = crc32(crc, baseAddress.assumingMemoryBound(to: UInt8.self), uInt(buffer.count))
        }
    }
    return UInt32(crc)
}

struct VerificationResult {
    var matchCount: Int
    var missing: [String]
    var mismatched: [String]
}

func verifyAppFiles(appBundleURL: URL, ipaURL: URL) throws -> VerificationResult {
    let entries = try readZipEntries(from: ipaURL)

    // We only care about files in Payload/*.app/
    let payloadPrefix = "Payload/"
    guard let appDirEntry = entries.first(where: { $0.name.hasPrefix(payloadPrefix) && $0.name.hasSuffix(".app/") && $0.name.split(separator: "/").count == 2 }) else {
        throw NSError(domain: "Verify", code: 1, userInfo: [NSLocalizedDescriptionKey: "Invalid IPA structure"])
    }
    let appPrefix = appDirEntry.name

    var matchCount = 0
    var missing = [String]()
    var mismatched = [String]()

    for entry in entries {
        guard !entry.isDirectory, entry.name.hasPrefix(appPrefix) else { continue }

        // Remove appPrefix to get relative path within .app bundle
        let relPath = String(entry.name.dropFirst(appPrefix.count))
        let fileURL = appBundleURL.appendingPathComponent(relPath)

        if let attrs = try? FileManager.default.attributesOfItem(atPath: fileURL.path(percentEncoded: false)),
           let fileSize = attrs[.size] as? NSNumber {
            if UInt32(fileSize.uint64Value) != entry.size {
                mismatched.append(relPath)
            } else if let crc = fileCRC32(url: fileURL) {
                if crc != entry.crc32 {
                    mismatched.append(relPath)
                } else {
                    matchCount += 1
                }
            } else {
                missing.append(relPath)
            }
        } else {
            missing.append(relPath)
        }
    }

    return VerificationResult(matchCount: matchCount, missing: missing, mismatched: mismatched)
}
