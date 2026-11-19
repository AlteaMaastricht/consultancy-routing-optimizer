#!/usr/bin/env python3
"""Synthetic instance generator for the consultant routing / team-orienteering problem.

Produces ``orders.txt`` and ``drivingtimes.txt`` in exactly the file formats the solvers read,
so the code can be run and tested without the course-provided instance (which is not
redistributable and is not part of this repository).

Model of the synthetic data (deliberately simple, not a replica of the course data):
* ``--nodes`` locations uniformly at random in a square of ``--area-km`` x ``--area-km``;
  the depot is node ``--depot`` (default 251, the node the solvers hard-code) and is moved to
  the centre of the square.
* driving time between two nodes = round(Euclidean distance / ``--speed-kmh``) in seconds;
  the matrix is symmetric with a zero diagonal (rounding can break the triangle
  inequality by at most 1 s).
* ``--orders`` orders, each at a uniformly random non-depot node; service duration uniform in
  [5, 13] minutes and profit uniform in [5, 44] euro (the value ranges were chosen to be similar
  in scale to the course instance; nothing else is copied from it).
* each of the ``--students`` consultants is eligible for an order with probability
  ``--eligibility`` (at least one consultant is always eligible).

The solvers in this repository hard-code 20 consultants and depot node 251, so keep
``--students 20`` and ``--nodes`` > 251 when generating data for them.

Usage:
  python3 tools/make_instance.py --out-dir data/synthetic --orders 120 --nodes 300 --seed 1
"""
from __future__ import annotations

import argparse
import math
import os
import random
import sys
from typing import List, Optional, Sequence


def generate(n_orders: int = 120, n_nodes: int = 300, students: int = 20, depot: int = 251,
             seed: int = 1, area_km: float = 60.0, speed_kmh: float = 70.0,
             eligibility: float = 0.5):
    """Return (orders_text, times_text) for a reproducible synthetic instance."""
    if n_nodes <= depot:
        raise ValueError(f"--nodes must be larger than the depot index {depot}")
    if n_orders < 1:
        raise ValueError("--orders must be at least 1")
    rng = random.Random(seed)
    pts = [(rng.uniform(0, area_km), rng.uniform(0, area_km)) for _ in range(n_nodes)]
    pts[depot] = (area_km / 2.0, area_km / 2.0)
    sec_per_km = 3600.0 / speed_kmh
    times: List[List[int]] = [[0] * n_nodes for _ in range(n_nodes)]
    for i in range(n_nodes):
        for j in range(i + 1, n_nodes):
            t = int(round(math.dist(pts[i], pts[j]) * sec_per_km))
            times[i][j] = times[j][i] = t
    header = ["Order", "City", "Node ID", "Coordinate X", "Coordinate Y", "Duration (min)", "Profit (eur)"]
    header += [f"Student {s + 1}" for s in range(students)]
    lines = ["\t".join(header)]
    candidates = [v for v in range(n_nodes) if v != depot]
    order_ids = rng.sample(range(10, 10 * n_orders + 10), n_orders)
    for k, oid in enumerate(order_ids):
        node = rng.choice(candidates)
        x, y = pts[node]
        flags = [1 if rng.random() < eligibility else 0 for _ in range(students)]
        if not any(flags):
            flags[rng.randrange(students)] = 1
        row = [str(oid), f"SYN{k:04d}", str(node), str(int(x * 1000)), str(int(y * 1000)),
               str(rng.randint(5, 13)), str(rng.randint(5, 44))] + [str(f) for f in flags]
        lines.append("\t".join(row))
    orders_text = "\n".join(lines) + "\n"
    times_text = "\n".join("\t".join(str(v) for v in row) for row in times) + "\n"
    return orders_text, times_text


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="Generate a synthetic orders.txt + drivingtimes.txt pair.")
    ap.add_argument("--out-dir", default=".")
    ap.add_argument("--orders", type=int, default=120)
    ap.add_argument("--nodes", type=int, default=300)
    ap.add_argument("--students", type=int, default=20)
    ap.add_argument("--depot", type=int, default=251)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--area-km", type=float, default=60.0)
    ap.add_argument("--speed-kmh", type=float, default=70.0)
    ap.add_argument("--eligibility", type=float, default=0.5)
    a = ap.parse_args(argv)
    try:
        orders_text, times_text = generate(a.orders, a.nodes, a.students, a.depot, a.seed,
                                           a.area_km, a.speed_kmh, a.eligibility)
    except ValueError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    os.makedirs(a.out_dir, exist_ok=True)
    with open(os.path.join(a.out_dir, "orders.txt"), "w") as fh:
        fh.write(orders_text)
    with open(os.path.join(a.out_dir, "drivingtimes.txt"), "w") as fh:
        fh.write(times_text)
    print(f"wrote {a.orders} orders and a {a.nodes}x{a.nodes} driving-time matrix to {a.out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
