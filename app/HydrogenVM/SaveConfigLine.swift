import Foundation

/// Formatting around a single INI value. Unchanged lines are never regenerated.
struct SaveConfigLine {
    let key: String
    let value: String
    let prefix: String
    let suffix: String
    let quote: Character?

    init?(_ line: String) {
        let trimmed = line.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty, !["#", ";", "//", "["].contains(where: trimmed.hasPrefix),
              let delimiter = line.firstIndex(where: { $0 == "=" || $0 == ":" }) else { return nil }
        key = String(line[..<delimiter]).trimmingCharacters(in: .whitespaces)
        guard !key.isEmpty else { return nil }
        var start = line.index(after: delimiter)
        while start < line.endIndex && (line[start] == " " || line[start] == "\t") { start = line.index(after: start) }
        if start < line.endIndex && (line[start] == "\"" || line[start] == "'") {
            let q = line[start]
            let valueStart = line.index(after: start)
            var end = valueStart
            var escaped = false
            while end < line.endIndex {
                if !escaped && line[end] == q { break }
                if !escaped && line[end] == "\\" { escaped = true } else { escaped = false }
                end = line.index(after: end)
            }
            guard end < line.endIndex else { return nil }
            prefix = String(line[..<valueStart])
            suffix = String(line[end...])
            value = String(line[valueStart..<end])
            quote = q
        } else {
            var end = line.endIndex
            var cursor = start
            while cursor < line.endIndex {
                if (line[cursor] == ";" || line[cursor] == "#") && cursor > start && line[line.index(before: cursor)].isWhitespace {
                    end = cursor; break
                }
                cursor = line.index(after: cursor)
            }
            while end > start && line[line.index(before: end)].isWhitespace { end = line.index(before: end) }
            prefix = String(line[..<start])
            suffix = String(line[end...])
            value = String(line[start..<end])
            quote = nil
        }
    }

    func replacingValue(_ newValue: String) throws -> String {
        let candidate = prefix + newValue + suffix
        guard !newValue.contains(where: \.isNewline), let check = SaveConfigLine(candidate),
              check.value == newValue, check.suffix == suffix else {
            throw NSError(domain: "HydrogenVM.ConfigEditor", code: 1,
                userInfo: [NSLocalizedDescriptionKey: "This value needs escaping or changes the config syntax. Use the raw text editor to enter it."])
        }
        return candidate
    }
}
