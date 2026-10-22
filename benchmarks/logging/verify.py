#!/usr/bin/env python3
"""Checks a JSON-lines log written by benchmarks/logging (any logger).

usage: verify.py <file> <records>

Every line must be a JSON object with the record's properties at the top
level: Method, Path, Status, Elapsed (record i: methods[i%4],
/api/items/<i%1000>, statuses[i%5], (i%100)*0.25) and the two context
properties Service=bench, RequestId=req-1. Prints "ok <records>" or exits 1.
"""
import json
import sys

METHODS = ["GET", "POST", "PUT", "DELETE"]
STATUSES = [200, 201, 404, 500, 302]


def main():
    path, n = sys.argv[1], int(sys.argv[2])
    count = 0
    with open(path, "rb") as f:
        for i, line in enumerate(f):
            count += 1
            # parse every line for small logs, every 997th for large ones
            if n > 200000 and i % 997 != 0 and i != n - 1:
                continue
            e = json.loads(line)
            want = {
                "Method": METHODS[i % 4],
                "Path": "/api/items/%d" % (i % 1000),
                "Status": STATUSES[i % 5],
                "Service": "bench",
                "RequestId": "req-1",
            }
            for k, v in want.items():
                if e.get(k) != v:
                    sys.exit("line %d: %s = %r, want %r" % (i + 1, k, e.get(k), v))
            if abs(float(e.get("Elapsed")) - (i % 100) * 0.25) > 1e-9:
                sys.exit("line %d: Elapsed = %r" % (i + 1, e.get("Elapsed")))
    if count != n:
        sys.exit("%d lines, want %d" % (count, n))
    print("ok", n)


main()
