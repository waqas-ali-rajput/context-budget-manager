"""Draws the scaling charts from results/bench_scaling.csv as standalone SVG files.

Standard library only (csv, math). Run from the cep/ folder:  python scripts/plot_scaling.py
"""
import csv
import math

SRC = "results/bench_scaling.csv"
COLORS = {"recency": "#6b7f99", "relevance": "#c47f12", "density": "#7b4fa0", "hybrid": "#0f6b72"}
SERIES = [("recency", "merge-sort"), ("recency", "heap"), ("relevance", "merge-sort"), ("relevance", "heap"),
          ("density", "merge-sort"), ("density", "heap"), ("hybrid", "heap")]


def load():
    with open(SRC, newline="") as f:
        return list(csv.DictReader(f))


def chart(path, title, xlabel, points, xs):
    """points: {(policy, engine): [(x, y), ...]} plotted on log-log axes."""
    W, H, L, R, T, B = 640, 360, 70, 170, 34, 50
    ys = [y for pts in points.values() for _, y in pts]
    x0, x1 = math.log10(min(xs)), math.log10(max(xs))
    y0, y1 = math.floor(math.log10(min(ys))), math.ceil(math.log10(max(ys)))

    def px(x):
        return L + (math.log10(x) - x0) / (x1 - x0) * (W - L - R)

    def py(y):
        return T + (1 - (math.log10(y) - y0) / (y1 - y0)) * (H - T - B)

    out = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {W} {H}" font-family="Segoe UI, Arial" font-size="11">',
           f'<rect width="{W}" height="{H}" fill="#fff"/>',
           f'<text x="{L}" y="20" font-size="13" font-weight="600" fill="#13233a">{title}</text>']
    for e in range(y0, y1 + 1):
        y = py(10 ** e)
        out.append(f'<line x1="{L}" y1="{y:.1f}" x2="{W - R}" y2="{y:.1f}" stroke="#e1e7ec"/>')
        label = f"{10 ** e:g}" if e >= 0 else f"{10 ** e:.1f}"
        out.append(f'<text x="{L - 6}" y="{y + 4:.1f}" text-anchor="end" fill="#334">{label}</text>')
    for x in xs:
        out.append(f'<line x1="{px(x):.1f}" y1="{T}" x2="{px(x):.1f}" y2="{H - B}" stroke="#e1e7ec"/>')
        out.append(f'<text x="{px(x):.1f}" y="{H - B + 16}" text-anchor="middle" fill="#334">{x:,}</text>')
    out.append(f'<line x1="{L}" y1="{H - B}" x2="{W - R}" y2="{H - B}" stroke="#13233a"/>')
    out.append(f'<line x1="{L}" y1="{T}" x2="{L}" y2="{H - B}" stroke="#13233a"/>')
    out.append(f'<text x="{(L + W - R) / 2}" y="{H - 12}" text-anchor="middle" fill="#13233a">{xlabel}</text>')
    out.append(f'<text transform="translate(18 {(T + H - B) / 2}) rotate(-90)" text-anchor="middle" fill="#13233a">'
               'median time per selection (µs, log)</text>')
    for i, key in enumerate(SERIES):
        pts = points.get(key)
        if not pts:
            continue
        policy, engine = key
        dash = ' stroke-dasharray="5 3"' if engine == "merge-sort" else ""
        d = " ".join(f"{px(x):.1f},{py(y):.1f}" for x, y in pts)
        out.append(f'<polyline points="{d}" fill="none" stroke="{COLORS[policy]}" stroke-width="2"{dash}/>')
        for x, y in pts:
            out.append(f'<circle cx="{px(x):.1f}" cy="{py(y):.1f}" r="2.6" fill="{COLORS[policy]}"/>')
        ly = T + 8 + i * 18
        out.append(f'<line x1="{W - R + 12}" y1="{ly}" x2="{W - R + 36}" y2="{ly}" stroke="{COLORS[policy]}" '
                   f'stroke-width="2"{dash}/>')
        out.append(f'<text x="{W - R + 42}" y="{ly + 4}" fill="#13233a">{policy} / {engine}</text>')
    out.append("</svg>")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(out))


def main():
    rows = load()
    by_n, by_b = {}, {}
    for r in rows:
        key = (r["policy"], r["engine"])
        if r["b_ctx"] == "1024":
            by_n.setdefault(key, []).append((int(r["n"]), float(r["median_us"])))
        if r["n"] == "10000":
            by_b.setdefault(key, []).append((int(r["b_ctx"]), float(r["median_us"])))
    for pts in list(by_n.values()) + list(by_b.values()):
        pts.sort()
    chart("results/scaling_n.svg", "Selection time vs number of candidates n (B_CTX = 1024)", "candidates n (log)",
          by_n, [100, 1000, 10000, 100000])
    chart("results/scaling_k.svg", "Selection time vs budget at n = 10,000 (larger budget = more items k)",
          "B_CTX tokens (log)", by_b, [256, 1024, 4096, 16384, 65536])
    print("wrote results/scaling_n.svg and results/scaling_k.svg")


if __name__ == "__main__":
    main()
