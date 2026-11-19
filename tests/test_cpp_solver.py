"""End-to-end tests of the C++ solver on synthetic instances (stdlib unittest only).

Needs a built binary: ``SA_BIN=build/sa python3 -m unittest discover -s tests``.
Without SA_BIN (and without ./build/sa) the solver tests are skipped.
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


def write_instance(directory, n_orders=150, n_nodes=300, seed=21):
    orders_text, times_text = make_instance.generate(n_orders=n_orders, n_nodes=n_nodes, seed=seed)
    with open(os.path.join(directory, "orders.txt"), "w") as fh:
        fh.write(orders_text)
    with open(os.path.join(directory, "drivingtimes.txt"), "w") as fh:
        fh.write(times_text)


@unittest.skipUnless(HAVE_BIN, "build the solver first (cmake) or set SA_BIN")
class SolverTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        write_instance(self.tmp)
        self.inst = vs.load_instance(os.path.join(self.tmp, "orders.txt"), os.path.join(self.tmp, "drivingtimes.txt"))

    def tearDown(self):
        shutil.rmtree(self.tmp, ignore_errors=True)

    def run_sa(self, *args, threads=2, cwd=None, env_extra=None):
        env = dict(os.environ, SA_THREADS=str(threads))
        env.update(env_extra or {})
        return subprocess.run([os.path.abspath(BIN), *args], cwd=cwd or self.tmp, env=env,
                              capture_output=True, text=True, timeout=120)

    def validate(self, path):
        with open(path) as fh:
            return vs.validate(self.inst, fh.read().splitlines())

    def test_short_run_is_feasible_and_claim_matches(self):
        proc = self.run_sa("3", "", "42")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertNotIn("self-check failed", proc.stdout)
        self.assertNotIn("ERROR", proc.stdout)
        rep = self.validate(os.path.join(self.tmp, "output.txt"))
        self.assertTrue(rep.feasible, rep.errors)
        self.assertTrue(rep.claim_matches, rep.errors)
        self.assertGreater(rep.net_profit, 0)

    def test_warm_start_never_gets_worse(self):
        self.assertEqual(self.run_sa("3", "", "1").returncode, 0)
        warm = os.path.join(self.tmp, "warm.txt")
        shutil.copy(os.path.join(self.tmp, "output.txt"), warm)
        before = self.validate(warm).net_profit
        proc = self.run_sa("2", warm, "2")
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("Warm start from", proc.stdout)
        after = self.validate(os.path.join(self.tmp, "output.txt"))
        self.assertTrue(after.feasible, after.errors)
        self.assertGreaterEqual(after.net_profit, before)

    def test_unknown_order_in_warm_start_is_ignored(self):
        warm = os.path.join(self.tmp, "bad_warm.txt")
        with open(warm, "w") as fh:
            fh.write("0\n1\n99999999\n" + "0\n" * 19)
        proc = self.run_sa("2", warm, "3", threads=1)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("unknown order", proc.stderr)
        self.assertTrue(self.validate(os.path.join(self.tmp, "output.txt")).feasible)

    def test_missing_input_exits_with_error(self):
        empty = tempfile.mkdtemp()
        try:
            proc = self.run_sa("1", "", "1", cwd=empty)
            self.assertEqual(proc.returncode, 1)
            self.assertIn("cannot open", proc.stderr)
        finally:
            shutil.rmtree(empty, ignore_errors=True)

    def test_data_dir_environment_variable(self):
        out_dir = tempfile.mkdtemp()
        try:
            proc = self.run_sa("2", "", "4", cwd=out_dir, env_extra={"SA_DATA_DIR": self.tmp})
            self.assertEqual(proc.returncode, 0, proc.stderr)
            self.assertTrue(self.validate(os.path.join(out_dir, "output.txt")).feasible)
        finally:
            shutil.rmtree(out_dir, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
