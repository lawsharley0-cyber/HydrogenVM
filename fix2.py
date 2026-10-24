with open("app/HydrogenVM/FileBrowser.swift", "r") as f:
    content = f.read()

# Let's break down the complex ForEach structure
search = """                        Section(cat.rawValue) {
                            ForEach(group) { entry in
                                Button { editingFile = entry.item } label: {
                                    HStack(spacing: 12) {
                                        Image(systemName: entry.kind.symbol)
                                            .font(.title3)
                                            .foregroundStyle(.tint)
                                            .frame(width: 28)
                                        VStack(alignment: .leading, spacing: 2) {
                                            Text(entry.item.name)
                                                .font(.body.weight(.medium))
                                                .lineLimit(1)
                                            Text(entry.relativePath)
                                                .font(.caption2)
                                                .foregroundStyle(.secondary)
                                                .lineLimit(1)
                                        }
                                        Spacer()
                                        VStack(alignment: .trailing, spacing: 2) {
                                            Text(entry.kind.badgeLabel)
                                                .font(.caption2.weight(.semibold))
                                                .padding(.horizontal, 6)
                                                .padding(.vertical, 2)
                                                .background(.tint.opacity(0.15), in: Capsule())
                                                .foregroundStyle(.tint)
                                            Text(ByteCountFormatter.string(fromByteCount: entry.item.size, countStyle: .file))
                                                .font(.caption2)
                                                .foregroundStyle(.secondary)
                                        }
                                    }
                                }
                            }
                        }"""

# We can pull out the row view to its own struct to simplify type-checking
row_struct = """private struct SaveEditorDashboardRow: View {
    let entry: DiscoveredSaveFile
    let action: () -> Void
    var body: some View {
        Button(action: action) {
            HStack(spacing: 12) {
                Image(systemName: entry.kind.symbol)
                    .font(.title3)
                    .foregroundStyle(.tint)
                    .frame(width: 28)
                VStack(alignment: .leading, spacing: 2) {
                    Text(entry.item.name)
                        .font(.body.weight(.medium))
                        .lineLimit(1)
                    Text(entry.relativePath)
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
                Spacer()
                VStack(alignment: .trailing, spacing: 2) {
                    Text(entry.kind.badgeLabel)
                        .font(.caption2.weight(.semibold))
                        .padding(.horizontal, 6)
                        .padding(.vertical, 2)
                        .background(.tint.opacity(0.15), in: Capsule())
                        .foregroundStyle(.tint)
                    Text(ByteCountFormatter.string(fromByteCount: entry.item.size, countStyle: .file))
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }
            }
        }
    }
}
"""

replacement = """                        Section(cat.rawValue) {
                            ForEach(group) { entry in
                                SaveEditorDashboardRow(entry: entry) {
                                    editingFile = entry.item
                                }
                            }
                        }"""

content = content.replace(search, replacement)
content = content + "\n" + row_struct

with open("app/HydrogenVM/FileBrowser.swift", "w") as f:
    f.write(content)
