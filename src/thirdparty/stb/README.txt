stb_truetype.h v1.26 from https://github.com/nothings/stb
Pinned upstream commit: a7d4c2175630e1ea5b6f8dd00ab96fed0b97b96d
Unmodified. Dual MIT/public-domain license included at the end of the header.
Used for custom TrueType metrics, cmap and outline decoding; HydrogenVM's
existing rasterizer draws the outlines. stb does not provide a hardened font
parser; table-range checks reject basic truncation, not every malformed font.
