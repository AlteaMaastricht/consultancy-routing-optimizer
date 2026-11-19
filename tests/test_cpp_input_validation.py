"""Regression tests for input handling of the C++ solver (Phase 2 fixes, synthetic inputs only).

Each test reproduces a bug found in the audit of the original src/sa.cpp:
  1. malformed warm-start file -> out-of-bounds write / silent rejection
  2. malformed numeric input -> abort (uncaught std::invalid_argument) or silent truncation
  3. depot node 251 missing from the driving-time matrix -> silently wrong objective
  4. single-order instance -> integer division by zero in the neighbour-list precomputation
plus related hardening (order node outside the matrix, too many orders, uint16 overflow).

Needs a built binary: ``SA_BIN=build/sa python3 -m unittest discover -s tests``.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))

import make_instance  # noqa: E402
import validate_solution as vs  # noqa: E402

BIN = os.environ.get("SA_BIN") or os.path.join(ROOT, "build", "sa")
HAVE_BIN = os.path.isfile(BIN) and os.access(BIN, os.X_OK)


@unittest.skipUnless(HAVE_BIN, "build the solver first (cmake) or set SA_BIN")
class InputValidationTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    # ---------------------------------------------------------------- helpers
    def write(self, orders_text, times_text):
        with open(os.path.join(self.tmp, "orders.txt"), "w") as fh:
            fh.write(orders_text)
        with open(os.path.join(self.tmp, "drivingtimes.txt"), "w") as fh:
            fh.write(times_text)

    def instance(self, **kw):
        params = dict(n_orders=60, n_nodes=260, seed=31)
        params.update(kw)
        orders_text, times_text = make_instance.generate(**params)
        self.write(orders_text, times_text)
        return orders_text, times_text

    def run_sa(self, *args, threads=1):
        env = dict(os.environ, SA_THREADS=str(threads))
        return subprocess.run([os.path.abspath(BIN), *args], cwd=self.tmp, env=env,
                              capture_output=True, text=True, timeout=120)

    def assert_valid_output(self):
        inst = vs.load_instance(os.path.join(self.tmp, "orders.txt"), os.path.join(self.tmp, "drivingtimes.txt"))
        with open(os.path.join(self.tmp, "output.txt")) as fh:
            rep = vs.validate(inst, fh.read().splitlines())
        self.assertTrue(rep.feasible, rep.errors)
        self.assertTrue(rep.claim_matches, rep.errors)

    def assert_input_error(self, proc, *needles):
        self.assertEqual(proc.returncode, 1, f"expected a clean input error (exit 1), got {proc.returncode}\n"
                                             f"stderr: {proc.stderr[-500:]}")
        for n in needles:
            self.assertIn(n, proc.stderr)

    def edit_order_field(self, orders_text, row, col, value):
        lines = orders_text.split("\n")
        cols = lines[row].split("\t")
        cols[col] = value
        lines[row] = "\t".join(cols)
        return "\n".join(lines)

    def order_ids(self, orders_text):
        return [l.split("\t")[0] for l in orders_text.split("\n")[1:] if l.strip()]

    # ---------------------------------------------------------------- bug 1: warm start
    def test_bug1_warm_start_route_longer_than_capacity_is_rejected(self):
        orders_text, _ = self.instance()
        ids = self.order_ids(orders_text)
        warm = os.path.join(self.tmp, "warm.txt")
        with open(warm, "w") as fh:
            fh.write("\n".join(["0", "2500"] + (ids * 50)[:2500] + ["0"] * 19) + "\n")
        proc = self.run_sa("1", warm, "1")
        self.assertEqual(proc.returncode, 0, proc.stderr[-500:])
        self.assertIn("route length", proc.stderr)
        self.assert_valid_output()

    def test_bug1_truncated_warm_start_is_reported(self):
        orders_text, _ = self.instance()
        warm = os.path.join(self.tmp, "warm.txt")
        with open(warm, "w") as fh:
            fh.write("0\n3\n" + self.order_ids(orders_text)[0] + "\n")
        proc = self.run_sa("1", warm, "1")
        self.assertEqual(proc.returncode, 0, proc.stderr[-500:])
        self.assertIn("malformed warm start", proc.stderr)
        self.assert_valid_output()

    def test_bug1_non_numeric_warm_start_is_reported(self):
        self.instance()
        warm = os.path.join(self.tmp, "warm.txt")
        with open(warm, "w") as fh:
            fh.write("0\n1\nabc\n" + "0\n" * 19)
        proc = self.run_sa("1", warm, "1")
        self.assertEqual(proc.returncode, 0, proc.stderr[-500:])
        self.assertIn("malformed warm start", proc.stderr)

    # ---------------------------------------------------------------- bug 2: numeric parsing
    def test_bug2_decimal_profit_is_rejected(self):
        orders_text, times_text = self.instance()
        self.write(self.edit_order_field(orders_text, 2, 6, "12.5"), times_text)
        self.assert_input_error(self.run_sa("1", "", "1"), "orders.txt", "invalid integer")

    def test_bug2_non_numeric_eligibility_is_rejected(self):
        orders_text, times_text = self.instance()
        self.write(self.edit_order_field(orders_text, 2, 26, "XX"), times_text)
        self.assert_input_error(self.run_sa("1", "", "1"), "orders.txt", "invalid integer")

    def test_bug2_empty_field_is_rejected(self):
        orders_text, times_text = self.instance()
        self.write(self.edit_order_field(orders_text, 2, 5, ""), times_text)
        self.assert_input_error(self.run_sa("1", "", "1"), "orders.txt")

    def test_bug2_eligibility_flag_must_be_0_or_1(self):
        orders_text, times_text = self.instance()
        self.write(self.edit_order_field(orders_text, 2, 10, "2"), times_text)
        self.assert_input_error(self.run_sa("1", "", "1"), "orders.txt")

    def test_bug2_non_numeric_matrix_entry_is_rejected(self):
        orders_text, times_text = self.instance()
        rows = times_text.split("\n")
        cols = rows[3].split("\t")
        cols[7] = "1.5"
        rows[3] = "\t".join(cols)
        self.write(orders_text, "\n".join(rows))
        self.assert_input_error(self.run_sa("1", "", "1"), "drivingtimes.txt")

    def test_bug2_non_square_matrix_is_rejected(self):
        orders_text, times_text = self.instance()
        rows = times_text.rstrip("\n").split("\n")
        rows[10] = rows[10].rsplit("\t", 1)[0]
        self.write(orders_text, "\n".join(rows) + "\n")
        self.assert_input_error(self.run_sa("1", "", "1"), "drivingtimes.txt")

    # ---------------------------------------------------------------- bug 3: depot node
    def test_bug3_missing_depot_node_is_an_error(self):
        # 120-node matrix: node 251 (the depot) does not exist
        orders_text, times_text = make_instance.generate(n_orders=40, n_nodes=120, depot=5, seed=3)
        self.write(orders_text, times_text)
        self.assert_input_error(self.run_sa("1", "", "1"), "depot node 251")

    def test_bug3_order_node_outside_matrix_is_an_error(self):
        orders_text, times_text = self.instance()
        self.write(self.edit_order_field(orders_text, 2, 2, "5000"), times_text)
        self.assert_input_error(self.run_sa("1", "", "1"), "orders.txt", "node")

    # ---------------------------------------------------------------- bug 4: tiny instances
    def test_bug4_single_order_instance_runs(self):
        self.instance(n_orders=1)
        proc = self.run_sa("1", "", "1")
        self.assertEqual(proc.returncode, 0, proc.stderr[-500:])
        self.assert_valid_output()

    def test_bug4_two_order_instance_runs(self):
        self.instance(n_orders=2)
        proc = self.run_sa("1", "", "1")
        self.assertEqual(proc.returncode, 0, proc.stderr[-500:])
        self.assert_valid_output()

    def test_bug4_no_orders_is_an_error(self):
        _, times_text = self.instance()
        header = "\t".join(["Order", "City", "Node ID", "X", "Y", "Duration (min)", "Profit (eur)"]
                           + [f"Student {s + 1}" for s in range(20)])
        self.write(header + "\n", times_text)
        self.assert_input_error(self.run_sa("1", "", "1"), "no orders")

    # ---------------------------------------------------------------- related hardening
    def test_too_many_orders_is_an_error(self):
        self.instance(n_orders=1300, n_nodes=260)
        self.assert_input_error(self.run_sa("1", "", "1"), "orders")

    def test_driving_time_above_uint16_range_is_an_error(self):
        orders_text, times_text = self.instance()
        rows = times_text.split("\n")
        c3 = rows[3].split("\t")
        c4 = rows[4].split("\t")
        c3[4] = c4[3] = "70000"
        rows[3], rows[4] = "\t".join(c3), "\t".join(c4)
        self.write(orders_text, "\n".join(rows))
        self.assert_input_error(self.run_sa("1", "", "1"), "drivingtimes.txt")


if __name__ == "__main__":
    unittest.main()
