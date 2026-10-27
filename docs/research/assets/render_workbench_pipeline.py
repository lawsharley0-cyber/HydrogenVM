from pathlib import Path
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch

OUT = Path(__file__).with_name("hydrogenvm_workbench_pipeline.png")
fig, ax = plt.subplots(figsize=(15, 7.2), dpi=180)
fig.patch.set_facecolor("#f7f9fc")
ax.set_facecolor("#f7f9fc")
ax.set_xlim(0, 15)
ax.set_ylim(0, 7.2)
ax.axis("off")

nodes = [
    (0.45, 4.3, 2.25, 1.2, "CAPTURE", "Whole-library scenarios\nBuild + sandbox + seed"),
    (3.15, 4.3, 2.25, 1.2, "TRIAGE", "Classify + fingerprint\nCluster + rank"),
    (5.85, 4.3, 2.25, 1.2, "REPRODUCE", "Replay + first divergence\nMinimize + red fixture"),
    (8.55, 4.3, 2.25, 1.2, "REPAIR", "Localized context\nBounded attempts"),
    (11.25, 4.3, 3.25, 1.2, "VALIDATE", "Full regression + held-out apps\nIndependent review → candidate PR"),
]
colors = ["#dcecff", "#e4e0ff", "#ffecd1", "#ffe0e0", "#dcf3e5"]
edges = ["#3973ac", "#6554a4", "#ad731f", "#a64b4b", "#27734a"]
for (x, y, w, h, title, body), color, edge in zip(nodes, colors, edges):
    patch = FancyBboxPatch((x, y), w, h, boxstyle="round,pad=0.04,rounding_size=0.12",
                           linewidth=1.8, edgecolor=edge, facecolor=color)
    ax.add_patch(patch)
    ax.text(x + 0.16, y + h - 0.25, title, fontsize=10, weight="bold", color=edge, va="top")
    ax.text(x + w/2, y + 0.43, body, fontsize=9.5, color="#182230", ha="center", va="center")
for i in range(len(nodes)-1):
    x, y, w, h, *_ = nodes[i]
    nx, ny, nw, nh, *_ = nodes[i+1]
    ax.add_patch(FancyArrowPatch((x+w+0.05, y+h/2), (nx-0.05, ny+nh/2),
                                 arrowstyle="-|>", mutation_scale=16, linewidth=1.6, color="#65758b"))

foundation = FancyBboxPatch((1.0, 1.35), 13.0, 1.5, boxstyle="round,pad=0.06,rounding_size=0.15",
                            linewidth=1.5, edgecolor="#52677e", facecolor="#ffffff")
ax.add_patch(foundation)
ax.text(1.3, 2.55, "SHARED EVIDENCE AND ORACLES", fontsize=10, weight="bold", color="#354a61", va="top")
ax.text(7.5, 2.05,
        "Versioned run schema  •  deterministic event trace  •  layered assertions  •  private artifacts\n"
        "Specifications / native calculations  →  real hardware  →  independent emulators  →  regression baselines",
        fontsize=10, color="#25364a", ha="center", va="center")
for x in [1.6, 4.3, 7.0, 9.7, 12.9]:
    ax.add_patch(FancyArrowPatch((x, 2.9), (x, 4.2), arrowstyle="-|>", mutation_scale=13,
                                 linewidth=1.2, color="#8291a3"))

ax.add_patch(FancyArrowPatch((14.25, 4.2), (6.95, 3.0), connectionstyle="arc3,rad=-0.23",
                             arrowstyle="-|>", mutation_scale=16, linewidth=1.5,
                             linestyle="--", color="#a64b4b"))
ax.text(11.4, 3.25, "failure returns evidence, never an automatic merge", fontsize=9,
        color="#8c3d3d", ha="center")
ax.text(0.45, 6.65, "HydrogenVM evidence-driven compatibility workbench", fontsize=20,
        weight="bold", color="#14263b", va="top")
ax.text(0.45, 6.15, "Automation spends effort on shared, reproducible root causes—not on repeatedly reading logs.",
        fontsize=11, color="#52677e", va="top")
ax.text(0.45, 0.45,
        "Hard boundaries: no title-specific shortcuts  •  no weakening tests  •  no proprietary artifacts in Git  •  no automatic merge",
        fontsize=10, color="#52677e")
plt.tight_layout(pad=0.6)
fig.savefig(OUT, bbox_inches="tight", facecolor=fig.get_facecolor())
print(OUT)
