with open("app/HydrogenVM/FileBrowser.swift", "r") as f:
    content = f.read()

# The error says "the compiler is unable to type-check this expression in reasonable time; try breaking up the expression into distinct sub-expressions"
# This happens in `SaveEditorDashboardView`, probably due to a complex conditional or UI structure.
# Let's see the context.

search = """                                            Text(entry.relativePath)"""
idx = content.find(search)
if idx != -1:
    print("Found context around error:")
    print(content[idx-500:idx+500])
