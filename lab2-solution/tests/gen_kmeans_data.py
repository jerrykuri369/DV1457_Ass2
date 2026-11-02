#!/usr/bin/env python3
"""Writes a synthetic kmeans-data.txt for testing (use the Canvas file for real runs).

1797 points (the size the sequential program expects) scattered around nine
centres, one "x y" pair per line, reproducible from a fixed seed.

Usage: python3 gen_kmeans_data.py [output] [points]
"""
import random
import sys


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "kmeans-data.txt"
    count = int(sys.argv[2]) if len(sys.argv) > 2 else 1797
    rng = random.Random(2023)
    centres = [(rng.uniform(-40, 40), rng.uniform(-40, 40)) for _ in range(9)]
    with open(out, "w") as f:
        for _ in range(count):
            cx, cy = rng.choice(centres)
            f.write(f"{rng.gauss(cx, 6.0):.6f} {rng.gauss(cy, 6.0):.6f}\n")


if __name__ == "__main__":
    main()
