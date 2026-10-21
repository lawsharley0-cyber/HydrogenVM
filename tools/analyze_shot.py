import sys
from PIL import Image

def analyze(path):
    im = Image.open(path)
    colors = im.getcolors(maxcolors=2000000)
    print(f"{path}: size={im.size}, unique colors={len(colors) if colors else '>2M'}")
    if colors:
        for count, color in sorted(colors, reverse=True)[:5]:
            print(f"  {count} pixels of {color}")

if __name__ == "__main__":
    analyze(sys.argv[1])
