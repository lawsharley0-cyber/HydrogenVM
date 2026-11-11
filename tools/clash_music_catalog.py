"""Generate a private menu-loop CSV overlay from the pinned bundle."""
import csv
import io
import json
from pathlib import Path


def header(bundle):
    original = (Path(bundle) / "res/csv_client/music.csv").read_text(encoding="utf-8-sig")
    rows = list(csv.reader(io.StringIO(original)))
    columns = rows[0]
    name, loop, fade = [columns.index(k) for k in ("Name", "Loop", "FadeOutTimeSec")]
    current = ""
    found = 0
    for row in rows[2:]:
        if not row:
            continue
        row.extend([""] * max(0, len(columns) - len(row)))
        if row[name]:
            current = row[name]
        if current == "MenuMusic":
            row[loop], row[fade] = "true", "0"
            found += 1
    if found != 3:
        raise ValueError("Pinned menu music group must have three variants")
    output = io.StringIO(newline="")
    csv.writer(output, lineterminator="\r\n").writerows(rows)
    return "static const char music_loop_csv[]=" + json.dumps(output.getvalue()) + ";\n"
