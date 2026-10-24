import SwiftUI
import UniformTypeIdentifiers

// MARK: - Binary / Hex Editor with Value Inspector & Cheat Scanner

@MainActor
final class ScannerSession: ObservableObject {
    static let shared = ScannerSession()
    var url: URL?
    var matches: [BinarySearchMatch] = []
    @Published var searchQuery = ""
    @Published var searchType: BinaryEditorView.SearchValueType = .int32LE
}

struct BinarySearchMatch: Identifiable {
    let offset: Int
    let preview: String
    var id: Int { offset }
}

struct BinaryEditorView: View {
    let url: URL
    @State private var data = Data()
    @State private var compression: SaveCompressionFormat = .none
    @State private var selectedOffset: Int = 0
    @Environment(FileEditorSession.self) private var document
    @State private var loaded = false
    @State private var error: String?

    // Value Search / Cheat Scanner
    @State private var showingScanner = false
    @StateObject private var scanner = ScannerSession.shared
    @State private var replaceValueText = ""
    @State private var isNextScan = false

    enum SearchValueType: String, CaseIterable, Identifiable {
        case int8 = "Int8"
        case uint8 = "UInt8"
        case int16LE = "Int16 (LE)"
        case int16BE = "Int16 (BE)"
        case uint16LE = "UInt16 (LE)"
        case uint16BE = "UInt16 (BE)"
        case int32LE = "Int32 (LE)"
        case int32BE = "Int32 (BE)"
        case uint32LE = "UInt32 (LE)"
        case uint32BE = "UInt32 (BE)"
        case int64LE = "Int64 (LE)"
        case int64BE = "Int64 (BE)"
        case uint64LE = "UInt64 (LE)"
        case uint64BE = "UInt64 (BE)"
        case float32LE = "Float32 (LE)"
        case float32BE = "Float32 (BE)"
        case float64LE = "Double (LE)"
        case float64BE = "Double (BE)"
        case ascii = "ASCII / UTF-8"
        case utf16LE = "UTF-16 (LE)"
        case hex = "Hex Bytes"
        case stringPrefix32LE = "String (Len Prefix 32LE)"
        var id: String { rawValue }
    }

    private let bytesPerRow = 16
    private let maxDisplayBytes = 131_072 // 128 KB window

    var body: some View {
        VStack(spacing: 0) {
            if compression != .none {
                HStack {
                    Image(systemName: "archivebox.fill")
                    Text("Decompressed \(compression.rawValue) (\(data.count) bytes) — will re-compress on save")
                    Spacer()
                }
                .font(.caption)
                .padding(.horizontal, 12)
                .padding(.vertical, 6)
                .background(Color.orange.opacity(0.15))
            }

            // Interactive Multi-Type Value Inspector Bar at `selectedOffset`
            if !data.isEmpty && selectedOffset < data.count {
                BinaryValueInspectorBar(data: $data, offset: $selectedOffset, totalCount: data.count) {
                    document.markChanged()
                }
                Divider()
            }

            // Hex Rows
            // Instead of showing the entire file or clamping to 128KB, we show a sliding window
            // centered around the selected offset to allow inspecting anywhere in large files.
            let windowSize = 4096 // 4KB window (256 rows)
            let halfWindow = windowSize / 2
            let windowStart = max(0, selectedOffset - halfWindow) / bytesPerRow * bytesPerRow
            let windowEnd = min(data.count, windowStart + windowSize)
            let rowCount = (windowEnd - windowStart + bytesPerRow - 1) / bytesPerRow

            List {
                if windowStart > 0 {
                    Text("... \(windowStart) bytes preceding ...").foregroundStyle(.secondary).font(.caption2).listRowInsets(EdgeInsets(top: 4, leading: 10, bottom: 4, trailing: 10))
                }
                ForEach(0..<rowCount, id: \.self) { row in
                    let base = windowStart + row * bytesPerRow
                    let end = min(base + bytesPerRow, windowEnd)
                    HexRow(
                        base: base,
                        slice: data[base..<end],
                        selectedOffset: $selectedOffset
                    ) { offset, byte in
                        data[offset] = byte
                        document.markChanged()
                    }
                    .listRowInsets(EdgeInsets(top: 2, leading: 10, bottom: 2, trailing: 10))
                }
                if windowEnd < data.count {
                    Text("... \(data.count - windowEnd) bytes following ...").foregroundStyle(.secondary).font(.caption2).listRowInsets(EdgeInsets(top: 4, leading: 10, bottom: 4, trailing: 10))
                }
            }
            .listStyle(.plain)
            .font(.system(.caption, design: .monospaced))
        }
        .navigationTitle(url.lastPathComponent)
        .safeAreaInset(edge: .top, spacing: 0) {
            HStack(spacing: 18) {
                Button {
                    showingScanner = true
                } label: {
                    Label("Value Scanner", systemImage: "magnifyingglass.circle")
                }
                ShareLink(item: url)
                Spacer()
            }
        }
        .modifier(FileEditorRegistration(save: save))
        .sheet(isPresented: $showingScanner) {
            NavigationStack {
                Form {
                    Section("Search Game Value (Coins, Gold, Stats, String)") {
                        Picker("Data Type", selection: $scanner.searchType) {
                            ForEach(SearchValueType.allCases) { t in
                                Text(t.rawValue).tag(t)
                            }
                        }
                        HStack {
                            TextField("Value to find (e.g. 500 or player name)", text: $scanner.searchQuery)
                                .font(.system(.body, design: .monospaced))
                                .autocorrectionDisabled()
                                .textInputAutocapitalization(.never)
                            if scanner.matches.isEmpty {
                                Button("New Scan") {
                                    runBinarySearch(isNextScan: false)
                                }
                                .buttonStyle(.borderedProminent)
                            } else {
                                Button("New Scan") {
                                    runBinarySearch(isNextScan: false)
                                }
                                .buttonStyle(.bordered)
                                Button("Next Scan") {
                                    runBinarySearch(isNextScan: true)
                                }
                                .buttonStyle(.borderedProminent)
                            }
                        }
                    }

                    if !scanner.matches.isEmpty {
                        Section("Batch Modify All \(scanner.matches.count) Matches") {
                            HStack {
                                TextField("New value", text: $replaceValueText)
                                    .font(.system(.body, design: .monospaced))
                                Button("Replace All") {
                                    replaceAllMatches()
                                }
                                .buttonStyle(.bordered)
                            }
                        }

                        Section("Matches (\(scanner.matches.count)) — Tap to Jump") {
                            ForEach(scanner.matches) { m in
                                Button {
                                    selectedOffset = m.offset
                                    showingScanner = false
                                } label: {
                                    HStack {
                                        Text(String(format: "0x%06X (%d)", m.offset, m.offset))
                                            .font(.system(.subheadline, design: .monospaced).weight(.semibold))
                                        Spacer()
                                        Text(m.preview)
                                            .font(.system(.caption, design: .monospaced))
                                            .foregroundStyle(.secondary)
                                    }
                                }
                            }
                        }
                    }
                }
                .navigationTitle("Binary Cheat / Value Scanner")
                .toolbar {
                    ToolbarItem(placement: .cancellationAction) {
                        Button("Close") { showingScanner = false }
                    }
                }
            }
        }
        .alert("Save Failed", isPresented: Binding(get: { error != nil }, set: { if !$0 { error = nil } })) {
            Button("OK", role: .cancel) { error = nil }
        } message: {
            Text(error ?? "")
        }
        .onAppear {
            if scanner.url != url {
                scanner.url = url
                scanner.matches = []
                scanner.searchQuery = ""
            }
            guard !loaded else { return }
            loaded = true
            do {
                let (payload, fmt) = try SaveCompressionFormat.decode(document.store.load())
                data = payload
                compression = fmt
                document.dirty = false
                document.ready = true
            } catch { self.error = error.localizedDescription }
        }
    }

    private func save() -> Bool {
        do {
            let out = try SaveCompressionFormat.encode(data, format: compression)
            try document.commit(out)
            return true
        } catch {
            document.saveError = error.localizedDescription
            return false
        }
    }

    private func encodePattern(query: String, type: SearchValueType) -> Data? {
        let q = query.trimmingCharacters(in: .whitespaces)
        switch type {
        case .int8:
            guard let v = Int8(q) else { return nil }
            var val = v
            return Data(bytes: &val, count: 1)
        case .uint8:
            guard let v = UInt8(q) else { return nil }
            var val = v
            return Data(bytes: &val, count: 1)
        case .int16LE:
            guard let v = Int16(q) else { return nil }
            var le = v.littleEndian
            return Data(bytes: &le, count: 2)
        case .int16BE:
            guard let v = Int16(q) else { return nil }
            var be = v.bigEndian
            return Data(bytes: &be, count: 2)
        case .uint16LE:
            guard let v = UInt16(q) else { return nil }
            var le = v.littleEndian
            return Data(bytes: &le, count: 2)
        case .uint16BE:
            guard let v = UInt16(q) else { return nil }
            var be = v.bigEndian
            return Data(bytes: &be, count: 2)
        case .int32LE:
            guard let v = Int32(q) else { return nil }
            var le = v.littleEndian
            return Data(bytes: &le, count: 4)
        case .int32BE:
            guard let v = Int32(q) else { return nil }
            var be = v.bigEndian
            return Data(bytes: &be, count: 4)
        case .uint32LE:
            guard let v = UInt32(q) else { return nil }
            var le = v.littleEndian
            return Data(bytes: &le, count: 4)
        case .uint32BE:
            guard let v = UInt32(q) else { return nil }
            var be = v.bigEndian
            return Data(bytes: &be, count: 4)
        case .int64LE:
            guard let v = Int64(q) else { return nil }
            var le = v.littleEndian
            return Data(bytes: &le, count: 8)
        case .int64BE:
            guard let v = Int64(q) else { return nil }
            var be = v.bigEndian
            return Data(bytes: &be, count: 8)
        case .uint64LE:
            guard let v = UInt64(q) else { return nil }
            var le = v.littleEndian
            return Data(bytes: &le, count: 8)
        case .uint64BE:
            guard let v = UInt64(q) else { return nil }
            var be = v.bigEndian
            return Data(bytes: &be, count: 8)
        case .float32LE:
            guard let v = Float(q) else { return nil }
            var bits = v.bitPattern.littleEndian
            return Data(bytes: &bits, count: 4)
        case .float32BE:
            guard let v = Float(q) else { return nil }
            var bits = v.bitPattern.bigEndian
            return Data(bytes: &bits, count: 4)
        case .float64LE:
            guard let v = Double(q) else { return nil }
            var bits = v.bitPattern.littleEndian
            return Data(bytes: &bits, count: 8)
        case .float64BE:
            guard let v = Double(q) else { return nil }
            var bits = v.bitPattern.bigEndian
            return Data(bytes: &bits, count: 8)
        case .ascii:
            return q.data(using: .utf8)
        case .utf16LE:
            return q.data(using: .utf16LittleEndian)
        case .hex:
            let clean = q.replacingOccurrences(of: " ", with: "")
            guard clean.count >= 2 && clean.count % 2 == 0 else { return nil }
            var bytes = Data()
            var idx = clean.startIndex
            while idx < clean.endIndex {
                let next = clean.index(idx, offsetBy: 2)
                guard let b = UInt8(clean[idx..<next], radix: 16) else { return nil }
                bytes.append(b)
                idx = next
            }
            return bytes
        case .stringPrefix32LE:
            guard let d = q.data(using: .utf8) else { return nil }
            var le = UInt32(d.count).littleEndian
            var out = Data(bytes: &le, count: 4)
            out.append(d)
            return out
        }
    }

    private func runBinarySearch(isNextScan: Bool = false) {
        guard let needle = encodePattern(query: scanner.searchQuery, type: scanner.searchType), !needle.isEmpty else {
            scanner.matches = []
            return
        }
        var found: [BinarySearchMatch] = []
        if isNextScan {
            for m in scanner.matches {
                guard m.offset + needle.count <= data.count else { continue }
                if data[m.offset..<(m.offset + needle.count)] == needle {
                    found.append(m)
                }
            }
        } else {
            var searchRange = data.startIndex..<data.endIndex
            while let r = data.range(of: needle, options: [], in: searchRange) {
                let off = r.lowerBound
                let ctxEnd = min(data.endIndex, off + 8)
                let hexPreview = data[off..<ctxEnd].map { String(format: "%02X", $0) }.joined(separator: " ")
                found.append(BinarySearchMatch(offset: off, preview: hexPreview))
                if found.count >= 1000 { break }
                searchRange = (r.lowerBound + 1)..<data.endIndex
            }
        }
        scanner.matches = found
    }

    private func replaceAllMatches() {
        guard let replacement = encodePattern(query: replaceValueText, type: scanner.searchType) else { return }
        for m in scanner.matches {
            guard m.offset + replacement.count <= data.count else { continue }
            data.replaceSubrange(m.offset..<(m.offset + replacement.count), with: replacement)
        }
        document.markChanged()
        runBinarySearch(isNextScan: false)
    }
}

private struct BinaryValueInspectorBar: View {
    @Binding var data: Data
    @Binding var offset: Int
    let totalCount: Int
    let onMutate: () -> Void

    @State private var editingType: String?
    @State private var editValueString = ""

    private func readLE<T: FixedWidthInteger>(_ type: T.Type) -> T? {
        let size = MemoryLayout<T>.size
        guard offset >= 0, offset + size <= data.count else { return nil }
        var val: T = 0
        _ = withUnsafeMutableBytes(of: &val) { dst in
            data.copyBytes(to: dst, from: offset..<(offset + size))
        }
        return T(littleEndian: val)
    }

    private func readBE<T: FixedWidthInteger>(_ type: T.Type) -> T? {
        let size = MemoryLayout<T>.size
        guard offset >= 0, offset + size <= data.count else { return nil }
        var val: T = 0
        _ = withUnsafeMutableBytes(of: &val) { dst in
            data.copyBytes(to: dst, from: offset..<(offset + size))
        }
        return T(bigEndian: val)
    }

    private func writeLE<T: FixedWidthInteger>(_ val: T) {
        let size = MemoryLayout<T>.size
        guard offset >= 0, offset + size <= data.count else { return }
        var le = val.littleEndian
        let bytes = withUnsafeBytes(of: &le) { Data($0) }
        data.replaceSubrange(offset..<(offset + size), with: bytes)
        onMutate()
    }

    private func writeBE<T: FixedWidthInteger>(_ val: T) {
        let size = MemoryLayout<T>.size
        guard offset >= 0, offset + size <= data.count else { return }
        var be = val.bigEndian
        let bytes = withUnsafeBytes(of: &be) { Data($0) }
        data.replaceSubrange(offset..<(offset + size), with: bytes)
        onMutate()
    }

    private var float32LEVal: Float? {
        guard let bits = readLE(UInt32.self) else { return nil }
        return Float(bitPattern: bits)
    }

    private var float32BEVal: Float? {
        guard let bits = readBE(UInt32.self) else { return nil }
        return Float(bitPattern: bits)
    }

    private var float64LEVal: Double? {
        guard let bits = readLE(UInt64.self) else { return nil }
        return Double(bitPattern: bits)
    }

    private var float64BEVal: Double? {
        guard let bits = readBE(UInt64.self) else { return nil }
        return Double(bitPattern: bits)
    }

    private var lenPrefixStringVal: String? {
        guard let len = readLE(UInt32.self) else { return nil }
        let total = 4 + Int(len)
        guard offset + total <= data.count else { return nil }
        let strData = data[(offset + 4)..<(offset + total)]
        return String(data: strData, encoding: .utf8)
    }

    private func writeLenPrefixString(_ val: String) {
        guard let strData = val.data(using: .utf8) else { return }
        guard let oldLen = readLE(UInt32.self) else { return }
        let oldTotal = 4 + Int(oldLen)
        guard offset + oldTotal <= data.count else { return }
        var le = UInt32(strData.count).littleEndian
        var newBytes = withUnsafeBytes(of: &le) { Data($0) }
        newBytes.append(strData)
        data.replaceSubrange(offset..<(offset + oldTotal), with: newBytes)
        onMutate()
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text(String(format: "Offset: 0x%06X (%d)", offset, offset))
                    .font(.caption.monospaced().weight(.bold))
                Spacer()
                Stepper("", value: $offset, in: 0...max(0, totalCount - 1))
                    .labelsHidden()
                    .controlSize(.mini)
            }
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 10) {
                    if let v = readLE(Int8.self) {
                        inspectorChip(label: "Int8", value: "\(v)") {
                            if let nv = Int8($0) { writeLE(nv) }
                        }
                    }
                    if let v = readLE(UInt8.self) {
                        inspectorChip(label: "UInt8", value: "\(v)") {
                            if let nv = UInt8($0) { writeLE(nv) }
                        }
                    }
                    if let v = readLE(Int16.self) {
                        inspectorChip(label: "Int16(LE)", value: "\(v)") {
                            if let nv = Int16($0) { writeLE(nv) }
                        }
                    }
                    if let v = readBE(Int16.self) {
                        inspectorChip(label: "Int16(BE)", value: "\(v)") {
                            if let nv = Int16($0) { writeBE(nv) }
                        }
                    }
                    if let v = readLE(UInt16.self) {
                        inspectorChip(label: "UInt16(LE)", value: "\(v)") {
                            if let nv = UInt16($0) { writeLE(nv) }
                        }
                    }
                    if let v = readBE(UInt16.self) {
                        inspectorChip(label: "UInt16(BE)", value: "\(v)") {
                            if let nv = UInt16($0) { writeBE(nv) }
                        }
                    }
                    if let v = readLE(Int32.self) {
                        inspectorChip(label: "Int32(LE)", value: "\(v)") {
                            if let nv = Int32($0) { writeLE(nv) }
                        }
                    }
                    if let v = readBE(Int32.self) {
                        inspectorChip(label: "Int32(BE)", value: "\(v)") {
                            if let nv = Int32($0) { writeBE(nv) }
                        }
                    }
                    if let v = readLE(UInt32.self) {
                        inspectorChip(label: "UInt32(LE)", value: "\(v)") {
                            if let nv = UInt32($0) { writeLE(nv) }
                        }
                    }
                    if let v = readBE(UInt32.self) {
                        inspectorChip(label: "UInt32(BE)", value: "\(v)") {
                            if let nv = UInt32($0) { writeBE(nv) }
                        }
                    }
                    if let v = readLE(Int64.self) {
                        inspectorChip(label: "Int64(LE)", value: "\(v)") {
                            if let nv = Int64($0) { writeLE(nv) }
                        }
                    }
                    if let v = readBE(Int64.self) {
                        inspectorChip(label: "Int64(BE)", value: "\(v)") {
                            if let nv = Int64($0) { writeBE(nv) }
                        }
                    }
                    if let v = readLE(UInt64.self) {
                        inspectorChip(label: "UInt64(LE)", value: "\(v)") {
                            if let nv = UInt64($0) { writeLE(nv) }
                        }
                    }
                    if let v = readBE(UInt64.self) {
                        inspectorChip(label: "UInt64(BE)", value: "\(v)") {
                            if let nv = UInt64($0) { writeBE(nv) }
                        }
                    }
                    if let f = float32LEVal {
                        inspectorChip(label: "Float32(LE)", value: String(format: "%.4g", f)) {
                            if let nv = Float($0) { writeLE(nv.bitPattern) }
                        }
                    }
                    if let f = float32BEVal {
                        inspectorChip(label: "Float32(BE)", value: String(format: "%.4g", f)) {
                            if let nv = Float($0) { writeBE(nv.bitPattern) }
                        }
                    }
                    if let d = float64LEVal {
                        inspectorChip(label: "Double(LE)", value: String(format: "%.6g", d)) {
                            if let nv = Double($0) { writeLE(nv.bitPattern) }
                        }
                    }
                    if let d = float64BEVal {
                        inspectorChip(label: "Double(BE)", value: String(format: "%.6g", d)) {
                            if let nv = Double($0) { writeBE(nv.bitPattern) }
                        }
                    }
                    if let s = lenPrefixStringVal {
                        inspectorChip(label: "String", value: "\(s)") {
                            writeLenPrefixString($0)
                        }
                    }
                }
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(Color.secondary.opacity(0.08))
    }

    @ViewBuilder
    private func inspectorChip(label: String, value: String, commit: @escaping (String) -> Void) -> some View {
        Button {
            editValueString = value
            editingType = label
        } label: {
            VStack(alignment: .leading, spacing: 1) {
                Text(label).font(.system(size: 9, weight: .semibold, design: .monospaced)).foregroundStyle(.secondary)
                Text(value).font(.system(.caption, design: .monospaced).weight(.medium)).foregroundStyle(.primary)
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
            .background(.background, in: RoundedRectangle(cornerRadius: 6))
        }
        .alert("Edit \(label) at 0x\(String(format: "%X", offset))",
               isPresented: Binding(get: { editingType == label }, set: { if !$0 { editingType = nil } })) {
            TextField(label, text: $editValueString)
                .keyboardType(.numbersAndPunctuation)
            Button("Write") { commit(editValueString); editingType = nil }
            Button("Cancel", role: .cancel) { editingType = nil }
        }
    }
}

private struct HexRow: View {
    let base: Int
    let slice: Data.SubSequence
    @Binding var selectedOffset: Int
    let onEdit: (Int, UInt8) -> Void

    @State private var editingOffset: Int?
    @State private var hexInput = ""

    var body: some View {
        HStack(spacing: 6) {
            Text(String(format: "%06X", base))
                .foregroundStyle(.secondary)
            HStack(spacing: 4) {
                ForEach(0..<16, id: \.self) { i in
                    let idx = slice.startIndex + i
                    if idx < slice.endIndex {
                        let b = slice[idx]
                        Text(String(format: "%02X", b))
                            .padding(.horizontal, 2)
                            .background(selectedOffset == idx ? Color.accentColor.opacity(0.3) : Color.clear,
                                        in: RoundedRectangle(cornerRadius: 3))
                            .onTapGesture {
                                selectedOffset = idx
                            }
                            .onLongPressGesture {
                                selectedOffset = idx
                                hexInput = String(format: "%02X", b)
                                editingOffset = idx
                            }
                    } else {
                        Text("  ")
                    }
                }
            }
            Spacer(minLength: 4)
            Text(asciiColumn)
                .foregroundStyle(.secondary)
        }
        .alert("Edit Byte", isPresented: Binding(get: { editingOffset != nil },
                                                 set: { if !$0 { editingOffset = nil } })) {
            TextField("Hex (00–FF)", text: $hexInput)
                .textInputAutocapitalization(.characters)
                .autocorrectionDisabled()
            Button("OK") {
                if let off = editingOffset,
                   let v = UInt8(hexInput.trimmingCharacters(in: .whitespaces), radix: 16) {
                    onEdit(off, v)
                }
                editingOffset = nil
            }
            Button("Cancel", role: .cancel) { editingOffset = nil }
        } message: {
            if let off = editingOffset {
                Text(String(format: "Offset 0x%06X", off))
            }
        }
    }

    private var asciiColumn: String {
        String(slice.map { (0x20...0x7E).contains($0) ? Character(UnicodeScalar($0)) : "." })
    }
}
