import Foundation
import zlib

enum SaveCompressionFormat: String {
    case none = "Uncompressed"
    case zlib = "Zlib"
    case gzip = "Gzip"

    static func detect(_ data: Data) -> SaveCompressionFormat {
        guard data.count >= 2 else { return .none }
        let b0 = data[data.startIndex], b1 = data[data.startIndex + 1]
        if b0 == 0x1F && b1 == 0x8B { return .gzip }
        if b0 & 0x0f == 8, b0 >> 4 <= 7, (Int(b0) * 256 + Int(b1)) % 31 == 0 { return .zlib }
        return .none
    }

    static func decompress(_ data: Data) -> (Data, SaveCompressionFormat)? {
        let fmt = detect(data)
        guard fmt != .none, !data.isEmpty else { return nil }
        var stream = z_stream()
        let windowBits: Int32 = (fmt == .gzip) ? (MAX_WBITS + 16) : MAX_WBITS
        guard inflateInit2_(&stream, windowBits, ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size)) == Z_OK else {
            return nil
        }
        defer { inflateEnd(&stream) }

        var out = Data(capacity: data.count * 3)
        let chunkSize = 32768
        var chunk = [UInt8](repeating: 0, count: chunkSize)

        let status: Int32 = data.withUnsafeBytes { rawBuf in
            guard let base = rawBuf.bindMemory(to: Bytef.self).baseAddress else { return Z_DATA_ERROR }
            stream.next_in = UnsafeMutablePointer<Bytef>(mutating: base)
            stream.avail_in = uInt(data.count)
            var ret: Int32 = Z_OK
            repeat {
                ret = chunk.withUnsafeMutableBufferPointer { outBuf in
                    stream.next_out = outBuf.baseAddress
                    stream.avail_out = uInt(chunkSize)
                    return inflate(&stream, Z_NO_FLUSH)
                }
                if ret == Z_OK || ret == Z_STREAM_END {
                    let have = chunkSize - Int(stream.avail_out)
                    if have > 0 { out.append(chunk, count: have) }
                } else {
                    break
                }
            } while ret != Z_STREAM_END
            return ret
        }
        // Unsupported concatenated members or custom trailing metadata must not
        // disappear when the editor recompresses only the first stream.
        guard status == Z_STREAM_END, stream.avail_in == 0 else { return nil }
        return (out, fmt)
    }

    static func decode(_ data: Data) throws -> (Data, SaveCompressionFormat) {
        let format = detect(data)
        guard format != .none else { return (data, .none) }
        guard let decoded = decompress(data) else { throw CocoaError(.fileReadCorruptFile) }
        return decoded
    }

    static func encode(_ data: Data, format: SaveCompressionFormat) throws -> Data {
        guard let encoded = compress(data, format: format) else { throw CocoaError(.fileWriteUnknown) }
        return encoded
    }

    static func compress(_ data: Data, format: SaveCompressionFormat) -> Data? {
        guard format != .none else { return data }
        var stream = z_stream()
        let windowBits: Int32 = (format == .gzip) ? (MAX_WBITS + 16) : MAX_WBITS
        guard deflateInit2_(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, windowBits, 8, Z_DEFAULT_STRATEGY,
                            ZLIB_VERSION, Int32(MemoryLayout<z_stream>.size)) == Z_OK else {
            return nil
        }
        defer { deflateEnd(&stream) }

        var out = Data()
        let chunkSize = 32768
        var chunk = [UInt8](repeating: 0, count: chunkSize)

        let status: Int32 = data.withUnsafeBytes { rawBuf in
            let base = rawBuf.bindMemory(to: Bytef.self).baseAddress
            stream.next_in = UnsafeMutablePointer<Bytef>(mutating: base)
            stream.avail_in = uInt(data.count)
            var ret: Int32 = Z_OK
            repeat {
                ret = chunk.withUnsafeMutableBufferPointer { outBuf in
                    stream.next_out = outBuf.baseAddress
                    stream.avail_out = uInt(chunkSize)
                    return deflate(&stream, Z_FINISH)
                }
                if ret == Z_OK || ret == Z_STREAM_END {
                    let have = chunkSize - Int(stream.avail_out)
                    if have > 0 { out.append(chunk, count: have) }
                } else {
                    break
                }
            } while ret != Z_STREAM_END
            return ret
        }
        return status == Z_STREAM_END ? out : nil
    }
}

