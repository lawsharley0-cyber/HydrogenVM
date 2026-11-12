"""Generate private context-specific music CSV overlays from the pinned bundle."""
import copy
import csv
import io
import json
from pathlib import Path


def header(bundle):
    text = (Path(bundle) / "res/csv_client/music.csv").read_text(encoding="utf-8-sig")
    original = list(csv.reader(io.StringIO(text)))
    columns = original[0]
    name, loop, fade, duration = [columns.index(k) for k in ("Name", "Loop", "FadeOutTimeSec", "DurationSec")]
    filename = columns.index("FileName")
    variants = []
    for mask in range(4):
        rows = copy.deepcopy(original)
        current = ""
        counts = {"MenuMusic": 0, "BattleMusic": 0}
        for row in rows[2:]:
            if not row:
                continue
            row.extend([""] * max(0, len(columns) - len(row)))
            if row[name]:
                current = row[name]
            if current == "MenuMusic":
                if mask & 1:
                    row[loop], row[fade], row[duration] = "true", "0", "@MUSIC_SECONDS@"
                counts[current] += 1
            elif current == "BattleMusic":
                if mask & 2:
                    row[filename] = f"music/modlab_battle_{counts[current]}.ogg"
                counts[current] += 1
        if set(counts.values()) != {3}:
            raise ValueError("Pinned menu and battle groups must each have three variants")
        out = io.StringIO(newline="")
        csv.writer(out, lineterminator="\r\n").writerows(rows)
        variants.append(out.getvalue())
    capacity = max(len(v.encode()) for v in variants) + 32
    return (f"#define MUSIC_CSV_CAPACITY {capacity}\n"
            "static const char *music_csv_variants[4]={" +
            ",".join(json.dumps(v) for v in variants) + "};\n")
