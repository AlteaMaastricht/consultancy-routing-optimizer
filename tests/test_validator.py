"""Unit tests for tools/validate_solution.py and tools/make_instance.py (stdlib unittest only)."""
import os
import sys
import tempfile
import unittest
from fractions import Fraction

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import make_instance  # noqa: E402
import validate_solution as vs  # noqa: E402

STUDENTS = 20
DEPOT = 251


def tiny_instance():
    """Hand-built instance: 3 orders at nodes 1, 2, 3; depot 251; all distances 600 s."""
    n = 260
    times = [[0 if i == j else 600 for j in range(n)] for i in range(n)]
    elig_all = frozenset(range(STUDENTS))
    orders = {
        101: vs.Order(101, 1, 10, 30, elig_all),
        102: vs.Order(102, 2, 10, 30, frozenset({0})),      # only consultant 1
        103: vs.Order(103, 3, 470, 600, elig_all),          # 470 min of service
    }
    return vs.Instance(orders, times, STUDENTS)


def solution(claimed, routes):
    lines = [str(claimed)]
    for s in range(STUDENTS):
        r = routes.get(s, [])
        lines.append(str(len(r)))
        lines.extend(str(o) for o in r)
    return lines


class ValidatorTests(unittest.TestCase):
    def test_empty_solution_is_feasible_with_zero_profit(self):
        rep = vs.validate(tiny_instance(), solution(0, {}))
        self.assertTrue(rep.feasible)
        self.assertTrue(rep.claim_matches)
        self.assertEqual(rep.net_profit, 0)

    def test_objective_is_profit_minus_60_eur_per_hour(self):
        # route depot->101->102->depot: driving 3*600 s = 30 min, service 20 min -> cost 50 eur
        rep = vs.validate(tiny_instance(), solution(10, {0: [101, 102]}))
        self.assertTrue(rep.feasible, rep.errors)
        self.assertEqual(rep.net_profit, Fraction(60 - 50))
        self.assertEqual(rep.exit_code, 0)

    def test_duplicate_order_is_infeasible(self):
        rep = vs.validate(tiny_instance(), solution(0, {0: [101], 1: [101]}))
        self.assertFalse(rep.feasible)
        self.assertTrue(any("more than once" in e for e in rep.errors))

    def test_ineligible_consultant_is_infeasible(self):
        rep = vs.validate(tiny_instance(), solution(0, {1: [102]}))
        self.assertFalse(rep.feasible)
        self.assertTrue(any("ineligible" in e for e in rep.errors))

    def test_shift_limit_includes_driving_and_service(self):
        # 470 min service + 20 min driving = 490 min > 480 min
        rep = vs.validate(tiny_instance(), solution(0, {0: [103]}))
        self.assertFalse(rep.feasible)
        self.assertTrue(any("works" in e for e in rep.errors))

    def test_unknown_order_is_infeasible(self):
        rep = vs.validate(tiny_instance(), solution(0, {0: [999]}))
        self.assertFalse(rep.feasible)

    def test_wrong_claim_is_reported(self):
        rep = vs.validate(tiny_instance(), solution(11, {0: [101, 102]}))
        self.assertTrue(rep.feasible)
        self.assertFalse(rep.claim_matches)
        self.assertEqual(rep.exit_code, 2)

    def test_truncated_file_raises_parse_error(self):
        with self.assertRaises(vs.ParseError):
            vs.validate(tiny_instance(), ["0", "1"])

    def test_trailing_lines_warn(self):
        rep = vs.validate(tiny_instance(), solution(0, {}) + ["5"])
        self.assertTrue(rep.warnings)


class GeneratorTests(unittest.TestCase):
    def test_generated_files_parse_and_are_deterministic(self):
        a = make_instance.generate(n_orders=30, n_nodes=260, seed=3)
        b = make_instance.generate(n_orders=30, n_nodes=260, seed=3)
        self.assertEqual(a, b)
        with tempfile.TemporaryDirectory() as d:
            for name, text in zip(("orders.txt", "drivingtimes.txt"), a):
                with open(os.path.join(d, name), "w") as fh:
                    fh.write(text)
            inst = vs.load_instance(os.path.join(d, "orders.txt"), os.path.join(d, "drivingtimes.txt"))
        self.assertEqual(len(inst.orders), 30)
        self.assertEqual(len(inst.times), 260)
        for i in range(260):
            self.assertEqual(inst.times[i][i], 0)
            for j in range(i):
                self.assertEqual(inst.times[i][j], inst.times[j][i])
        self.assertTrue(all(o.eligible for o in inst.orders.values()))
        self.assertTrue(all(o.node != DEPOT for o in inst.orders.values()))

    def test_rejects_matrix_without_depot(self):
        with self.assertRaises(ValueError):
            make_instance.generate(n_orders=5, n_nodes=200)


if __name__ == "__main__":
    unittest.main()
