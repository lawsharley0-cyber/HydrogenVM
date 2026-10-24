with open("app/HydrogenVM/FileBrowser.swift", "r") as f:
    text = f.read()
if "SaveEditorDashboardRow" in text and "the compiler is unable to type-check this expression" not in text:
    print("Replaced ok")
else:
    print("Not replaced")
