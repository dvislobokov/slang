#!/usr/bin/env python3
"""Runs benchmark binaries that report one timing row per stderr line.

Usage: bench-rows.py RUNS label=binary [label=binary...]

Each binary prints deterministic checksums on stdout and rows
"<unit> <name...> <value>" on stderr (unit: ms or us). The binaries run
RUNS times, alternating; stdout must be identical for all of them (else
exit 1). For every row the best (lowest) value over the runs is printed,
one column per label, plus "<label>/go" ratios when a label "go" exists.
"""
import subprocess
import sys


def main():
    runs = int(sys.argv[1])
    bins = [a.split("=", 1) for a in sys.argv[2:]]
    best = {}
    order = []
    ref_out = None
    for _ in range(runs):
        for label, path in bins:
            p = subprocess.run([path], capture_output=True, text=True)
            if p.returncode != 0:
                sys.exit(f"{label}: exit {p.returncode}\n{p.stderr}")
            if ref_out is None:
                ref_out = p.stdout
            elif p.stdout != ref_out:
                sys.exit(f"{label}: checksums differ\n--- first\n{ref_out}--- {label}\n{p.stdout}")
            for line in p.stderr.splitlines():
                f = line.split()
                if len(f) < 3 or f[0] not in ("ms", "us"):
                    continue
                key = " ".join(f[1:-1])
                if key not in order:
                    order.append(key)
                v = float(f[-1])
                cell = best.setdefault(key, {})
                if label not in cell or v < cell[label]:
                    cell[label] = v
                cell["unit"] = f[0]
    print("checksums (equal for all binaries):")
    print("  " + ref_out.strip().replace("\n", "\n  "))
    labels = [l for l, _ in bins]
    ratios = [l for l in labels if l != "go"] if "go" in labels else []
    head = f"{'row':<22} {'unit':<4} " + " ".join(f"{l:>12}" for l in labels)
    head += "".join(f" {l + '/go':>14}" for l in ratios)
    print(head)
    for key in order:
        cell = best[key]
        line = f"{key:<22} {cell['unit']:<4} " + " ".join(
            f"{cell.get(l, float('nan')):>12.0f}" for l in labels)
        for l in ratios:
            g = cell.get("go", 0)
            line += f" {cell.get(l, 0) / g:>14.2f}" if g else f" {'-':>14}"
        print(line)


if __name__ == "__main__":
    main()
