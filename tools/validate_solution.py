#!/usr/bin/env python3
"""Independent solution validator for the consultant routing / team-orienteering problem.

Written from the problem definition as implemented by the solvers in this repository
(it shares no code with them and is not the course-provided checker):

* ``orders.txt``: tab-separated, one header line, then one order per line with the columns
  ``Order ID, City, Node ID, X, Y, Duration (min), Profit (eur), Student 1 .. Student S``
  (the last S columns are 0/1 eligibility flags).
* ``drivingtimes.txt``: a square matrix of non-negative integer driving times in seconds,
  whitespace separated; row ``j`` / column ``i`` is the time from node ``j`` to node ``i``.
* solution file: first line = claimed net profit (integer euro); then, for each of the S
  consultants in order, a line with ``k`` followed by ``k`` order IDs in visiting order.

Constraints checked
  1. every order ID exists and is served at most once;
  2. the consultant serving an order is eligible for it;
  3. each shift starts and ends at the depot node and
     driving time + service time <= the shift limit (default 8 h = 28,800 s);
  4. the file contains exactly S route blocks (extra non-empty lines are reported).

Objective
  net profit = sum(order profit) - cost_per_hour * (driving + service seconds) / 3600,
  computed exactly with fractions.  The claimed value is accepted when it equals the
  exact net profit rounded down or up to an integer.

Exit codes: 0 = feasible and claim matches, 1 = infeasible, 2 = feasible but the claimed value
does not match, 3 = input could not be parsed.

Usage:
  python3 tools/validate_solution.py --orders orders.txt --times drivingtimes.txt solution.txt
  python3 tools/validate_solution.py ... --json      # machine-readable summary
"""
from __future__ import annotations

import argparse
import json
import math
import sys
from dataclasses import dataclass, field
from fractions import Fraction
from typing import Dict, List, Optional, Sequence


class ParseError(ValueError):
    """Raised when an input file does not follow the expected format."""


@dataclass
class Order:
    order_id: int
    node: int
    duration_min: int
    profit_eur: int
    eligible: frozenset


@dataclass
class Instance:
    orders: Dict[int, Order]
    times: List[List[int]]
    students: int


@dataclass
class Report:
    feasible: bool = True
    claimed: Optional[int] = None
    net_profit: Optional[Fraction] = None
    claim_matches: bool = False
    orders_served: int = 0
    errors: List[str] = field(default_factory=list)
    warnings: List[str] = field(default_factory=list)
    routes: List[dict] = field(default_factory=list)

    @property
    def exit_code(self) -> int:
        if not self.feasible:
            return 1
        if not self.claim_matches:
            return 2
        return 0

    def to_dict(self) -> dict:
        return {
            "feasible": self.feasible,
            "claimed": self.claimed,
            "net_profit_exact": None if self.net_profit is None else str(self.net_profit),
            "net_profit": None if self.net_profit is None else round(float(self.net_profit), 2),
            "claim_matches": self.claim_matches,
            "orders_served": self.orders_served,
            "errors": self.errors,
            "warnings": self.warnings,
            "routes": self.routes,
        }


def _int(token: str, what: str) -> int:
    try:
        return int(token.strip())
    except ValueError as exc:
        raise ParseError(f"{what}: expected an integer, got {token!r}") from exc


def read_orders(path: str, students: int) -> Dict[int, Order]:
    orders: Dict[int, Order] = {}
    with open(path, "r", newline="") as fh:
        lines = fh.read().splitlines()
    if not lines:
        raise ParseError(f"{path}: empty file")
    for lineno, raw in enumerate(lines[1:], start=2):
        line = raw.rstrip("\r")
        if not line.strip():
            continue
        cols = line.split("\t")
        if len(cols) < 7 + students:
            raise ParseError(f"{path}:{lineno}: expected at least {7 + students} tab-separated columns, got {len(cols)}")
        oid = _int(cols[0], f"{path}:{lineno} order id")
        flags = [_int(c, f"{path}:{lineno} eligibility") for c in cols[7:7 + students]]
        if any(f not in (0, 1) for f in flags):
            raise ParseError(f"{path}:{lineno}: eligibility flags must be 0 or 1")
        order = Order(
            order_id=oid,
            node=_int(cols[2], f"{path}:{lineno} node id"),
            duration_min=_int(cols[5], f"{path}:{lineno} duration"),
            profit_eur=_int(cols[6], f"{path}:{lineno} profit"),
            eligible=frozenset(i for i, f in enumerate(flags) if f == 1),
        )
        if order.duration_min < 0:
            raise ParseError(f"{path}:{lineno}: negative duration")
        if oid in orders:
            raise ParseError(f"{path}:{lineno}: duplicate order id {oid}")
        orders[oid] = order
    return orders


def read_times(path: str) -> List[List[int]]:
    rows: List[List[int]] = []
    with open(path, "r") as fh:
        for lineno, line in enumerate(fh, start=1):
            if not line.strip():
                continue
            rows.append([_int(t, f"{path}:{lineno}") for t in line.split()])
    n = len(rows)
    if n == 0:
        raise ParseError(f"{path}: empty matrix")
    for i, row in enumerate(rows):
        if len(row) != n:
            raise ParseError(f"{path}: row {i} has {len(row)} entries, expected {n} (matrix must be square)")
        if any(v < 0 for v in row):
            raise ParseError(f"{path}: row {i} contains a negative driving time")
    return rows


def load_instance(orders_path: str, times_path: str, students: int = 20) -> Instance:
    inst = Instance(read_orders(orders_path, students), read_times(times_path), students)
    n = len(inst.times)
    for o in inst.orders.values():
        if not 0 <= o.node < n:
            raise ParseError(f"order {o.order_id}: node {o.node} outside the {n}x{n} driving-time matrix")
    return inst


def validate(inst: Instance, solution_lines: Sequence[str], depot: int = 251,
             shift_seconds: int = 8 * 3600, cost_per_hour: int = 60) -> Report:
    rep = Report()
    if not 0 <= depot < len(inst.times):
        raise ParseError(f"depot node {depot} outside the driving-time matrix")
    tokens = [ln.strip() for ln in solution_lines]
    pos = 0

    def next_token(what: str) -> int:
        nonlocal pos
        while pos < len(tokens) and tokens[pos] == "":
            pos += 1
        if pos >= len(tokens):
            raise ParseError(f"solution file ended early while reading {what}")
        value = _int(tokens[pos], what)
        pos += 1
        return value

    rep.claimed = next_token("claimed net profit")
    seen: Dict[int, int] = {}
    total_profit = 0
    total_seconds = 0
    T = inst.times
    for s in range(inst.students):
        k = next_token(f"route length of consultant {s + 1}")
        if k < 0:
            raise ParseError(f"consultant {s + 1}: negative route length {k}")
        cur = depot
        drive = service = profit = 0
        route_ids = []
        for _ in range(k):
            oid = next_token(f"order id of consultant {s + 1}")
            route_ids.append(oid)
            order = inst.orders.get(oid)
            if order is None:
                rep.feasible = False
                rep.errors.append(f"consultant {s + 1}: unknown order id {oid}")
                continue
            if oid in seen:
                rep.feasible = False
                rep.errors.append(f"order {oid} served more than once (consultants {seen[oid] + 1} and {s + 1})")
            else:
                seen[oid] = s
            if s not in order.eligible:
                rep.feasible = False
                rep.errors.append(f"order {oid} served by ineligible consultant {s + 1}")
            drive += T[cur][order.node]
            service += order.duration_min * 60
            profit += order.profit_eur
            cur = order.node
        if k > 0:
            drive += T[cur][depot]
        shift = drive + service
        if shift > shift_seconds:
            rep.feasible = False
            rep.errors.append(f"consultant {s + 1} works {shift} s > {shift_seconds} s")
        total_profit += profit
        total_seconds += shift
        rep.routes.append({"consultant": s + 1, "orders": k, "drive_s": drive,
                           "service_s": service, "shift_s": shift, "profit_eur": profit})
    trailing = [t for t in tokens[pos:] if t]
    if trailing:
        rep.warnings.append(f"{len(trailing)} non-empty line(s) after the last route block were ignored")
    rep.orders_served = len(seen)
    rep.net_profit = Fraction(total_profit) - Fraction(cost_per_hour * total_seconds, 3600)
    lo, hi = math.floor(rep.net_profit), math.ceil(rep.net_profit)
    rep.claim_matches = rep.claimed in (lo, hi)
    if not rep.claim_matches:
        rep.errors.append(f"claimed {rep.claimed} but the recomputed net profit is {float(rep.net_profit):.2f}")
    return rep


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("solution", help="solution file to check")
    ap.add_argument("--orders", default="orders.txt")
    ap.add_argument("--times", default="drivingtimes.txt")
    ap.add_argument("--students", type=int, default=20)
    ap.add_argument("--depot", type=int, default=251)
    ap.add_argument("--shift-seconds", type=int, default=8 * 3600)
    ap.add_argument("--cost-per-hour", type=int, default=60)
    ap.add_argument("--json", action="store_true", help="print a JSON summary")
    args = ap.parse_args(argv)
    try:
        inst = load_instance(args.orders, args.times, args.students)
        with open(args.solution) as fh:
            rep = validate(inst, fh.read().splitlines(), args.depot, args.shift_seconds, args.cost_per_hour)
    except (OSError, ParseError) as exc:
        print(f"PARSE ERROR: {exc}", file=sys.stderr)
        return 3
    if args.json:
        out = rep.to_dict()
        out.pop("routes")
        print(json.dumps(out, indent=2))
    else:
        for e in rep.errors:
            print(f"ERROR: {e}")
        for w in rep.warnings:
            print(f"WARNING: {w}")
        status = "FEASIBLE" if rep.feasible else "INFEASIBLE"
        print(f"{status}: {rep.orders_served} orders served, net profit {float(rep.net_profit):.2f} "
              f"(claimed {rep.claimed}, {'matches' if rep.claim_matches else 'does NOT match'})")
    return rep.exit_code


if __name__ == "__main__":
    sys.exit(main())
