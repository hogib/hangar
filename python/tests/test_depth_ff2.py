# SPDX-License-Identifier: GPL-3.0-or-later
"""python/examples/depth_ff2.py, the second design of docs/09's adaptive depth
hold: the heave at rest follows the still rows and nothing else; the plant's
question holds no trim; the plant inverted is the oracle when the plant is
the simulator's own, zero with the vehicle still and zero for trims the rows
never held; the profile's spec is usable by a policy and its Arch is the
model's; the learner, on the example's own heave axis, finds the deadband's
side and leaves the trim to the integrator. numpy and PyTorch parts are
skipped without them."""
import os
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "python"))
sys.path.insert(0, os.path.join(REPO, "python", "examples"))
sys.path.insert(0, os.path.join(REPO, "tools", "sim"))

from hangar import dataset, learn  # noqa: E402

try:
    import numpy
    HAVE_NUMPY = True
except ImportError:
    HAVE_NUMPY = False
try:
    import torch
    HAVE_TORCH = HAVE_NUMPY
except ImportError:
    HAVE_TORCH = False

PROFILE = os.path.join(REPO, "profiles", "sim-depth-ff2")


def row(seq, accel_made, vel_z, integral, heave, flags=0):
    import depth_ff2
    pwm = depth_ff2.PWM_STOP - depth_ff2.PWM_FULL * heave   # heave up: below 1500 us
    return learn._Row(seq, seq * 20_000_000, b"\x01" * 16, flags, [9.0, vel_z, integral],
                      [accel_made, pwm, pwm, pwm, pwm, 7.0], False)


class Spec(unittest.TestCase):
    def test_usable_by_a_policy_and_waits_for_nothing(self):
        with open(os.path.join(PROFILE, "datasets", "depth_ff.dataset")) as f:
            spec = dataset.DatasetSpec.parse(f.read(), "depth_ff")
        self.assertEqual(spec.wait_ns(), 0)
        ok, why = spec.policy_usable()
        self.assertTrue(ok, why)

    def test_fields_name_the_command_lag_ticks_back(self):
        import depth_ff2
        self.assertEqual(depth_ff2.label_fields(0)[1], "thr.out.7.pwm[4]")
        self.assertEqual(depth_ff2.label_fields(3), ["made.accel[2]"] + ["thr.out.4.pwm[%d]" % c for c in (4, 5, 6, 7)]
                         + [depth_ff2.REST_SLOT])
        self.assertEqual(len(depth_ff2.label_fields()), depth_ff2.Y_REST + 1)


class Rest(unittest.TestCase):
    def test_follows_the_still_rows_and_nothing_else(self):
        import depth_ff2
        t = depth_ff2.RestTracker()
        r = row(1, 0.4, 0.2, -0.05, 0.3)
        t.row(r)
        self.assertEqual(r.y[depth_ff2.Y_REST], -0.05)    # no still row yet: the integrator
        for k in range(300):
            r = row(2 + k, 0.0, 0.0, -0.2, -0.085)
            t.row(r)
        self.assertAlmostEqual(r.y[depth_ff2.Y_REST], -0.085, places=4)
        for k in range(100):   # a step: moving, the command anywhere
            r = row(400 + k, -1.0, -0.3, -0.1, 0.4)
            t.row(r)
        self.assertAlmostEqual(r.y[depth_ff2.Y_REST], -0.085, places=4)
        r = row(600, 0.0, 0.0, 0.0, 0.5, flags=learn.MASKED)   # still, but nobody's command
        t.row(r)
        self.assertAlmostEqual(r.y[depth_ff2.Y_REST], -0.085, places=4)
        for k in range(400):   # heavier now: held somewhere else
            r = row(700 + k, 0.01, 0.0, 0.09, 0.088)
            t.row(r)
        self.assertAlmostEqual(r.y[depth_ff2.Y_REST], 0.088, places=3)
        self.assertAlmostEqual(t.lo, -0.085, places=4)
        self.assertAlmostEqual(t.hi, 0.088, places=3)

    def test_attached_every_row_of_the_buffer_has_it(self):
        import depth_ff2
        rp = learn.Replay(None)
        rp.add(row(1, 0.0, 0.0, 0.0, -0.08))
        s = learn._Stream(depth_ff2._SynthCodec(), 4, None, rp, "t")
        t = depth_ff2.RestTracker()
        t.attach(s)
        t.attach(s)   # once
        rp.add(row(2, 0.0, 0.0, 0.0, -0.08))
        rp.add(row(3, 2.0, 0.5, 0.0, 0.6))
        self.assertEqual([round(r.y[depth_ff2.Y_REST], 6) for r in rp.rows], [-0.08, -0.08, -0.08])
        lo, hi = t.applied(s)
        self.assertAlmostEqual(lo, -0.08, places=6)
        self.assertAlmostEqual(hi, 0.6, places=6)


@unittest.skipUnless(HAVE_NUMPY, "numpy is not installed")
class Plant(unittest.TestCase):
    def test_chunk_to_the_plant_s_question(self):
        import depth_ff2
        rows = [row(1, 0.0, 0.0, 0.0, -0.08), row(2, 0.5, 0.1, 0.0, -0.2), row(3, 0.5, 0.1, 0.0, -0.2, learn.MASKED)]
        for r in rows:
            r.y[depth_ff2.Y_REST] = -0.08
        c = learn.Chunk(numpy.array([r.x for r in rows], numpy.float32), numpy.array([r.y for r in rows], numpy.float32),
                        numpy.array([1.0, 1.0, 0.0], numpy.float32), numpy.ones(3, numpy.bool_))
        P, a, wt = depth_ff2.plant_xy(c)
        numpy.testing.assert_allclose(P, [[-0.08, -0.08, 0.0], [-0.2, -0.08, 0.1], [-0.2, -0.08, 0.1]], atol=1e-6)
        numpy.testing.assert_allclose(a, [0.0, 0.5, 0.5])
        numpy.testing.assert_allclose(wt, [1.0, depth_ff2.MOVING_WEIGHT, 0.0])

    def test_the_simulator_s_plant_holds_no_trim(self):
        import depth_ff2
        h = numpy.linspace(-0.3, 0.3, 61)
        P = numpy.stack([h, h, numpy.zeros_like(h)], -1)
        self.assertLess(float(numpy.abs(depth_ff2.sim_plant(P)).max()), 1e-12)
        # More heave up than at rest: it rises (z is down); sinking: drag slows it.
        self.assertLess(float(depth_ff2.sim_plant([[0.3, 0.1, 0.0]])[0]), -0.1)
        self.assertLess(float(depth_ff2.sim_plant([[0.1, 0.1, 0.3]])[0]), -0.1)

    def test_inverted_it_is_the_oracle(self):
        import depth_ff2
        X = depth_ff2.policy_samples(400, numpy.random.default_rng(0), i_range=(-0.2, 0.2))
        mine = depth_ff2.invert(depth_ff2.sim_plant, X)
        theirs = depth_ff2.oracle(X, bound=depth_ff2.BOUND)
        # The grid is 0.005 wide; at the deadband's edge a step of it is a newton.
        self.assertLess(float(numpy.median(numpy.abs(mine - theirs))), 0.006)
        self.assertLess(float(numpy.mean(numpy.abs(mine - theirs) > 0.03)), 0.05)

    def test_inverted_zero_at_rest_and_for_trims_never_held(self):
        import depth_ff2
        X = numpy.array([[0.0, 0.0, i] for i in (-0.2, -0.085, 0.0, 0.088, 0.2)], numpy.float32)
        self.assertLess(float(numpy.abs(depth_ff2.invert(depth_ff2.sim_plant, X)).max()), 0.006)
        X = numpy.array([[-1.0, 0.0, i] for i in (-0.085, -0.05, 0.0, 0.088)], numpy.float32)
        r = depth_ff2.invert(depth_ff2.sim_plant, X, trims=(-0.09, -0.04))
        self.assertGreater(float(r[0]), 0.1)
        self.assertGreater(float(r[1]), 0.1)
        self.assertEqual(r[2:].tolist(), [0.0, 0.0])    # trims outside the commands held: nothing


@unittest.skipUnless(HAVE_NUMPY, "numpy is not installed")
class Oracle(unittest.TestCase):
    def test_zero_with_the_vehicle_still(self):
        import depth_ff2
        X = numpy.array([[0.0, 0.0, i] for i in (-0.2, -0.085, -0.03, 0.0, 0.05, 0.088, 0.2)])
        self.assertLess(float(numpy.abs(depth_ff2.oracle(X)).max()), 1e-9)

    def test_pushes_the_way_the_controller_wants_and_across_the_deadband(self):
        import depth_ff2
        # Trimmed pushing down (a vehicle that floats); the controller wants up.
        up = float(depth_ff2.oracle(numpy.array([[-0.3, 0.0, -0.085]]))[0])
        down = float(depth_ff2.oracle(numpy.array([[0.3, 0.0, -0.085]]))[0])
        self.assertGreater(up, 0.14)     # the deadband is 0.14 wide, and it is on this side
        self.assertLess(down, 0.0)
        self.assertGreater(down, -0.14)

    def test_the_whole_command_never_rises_with_the_acceleration_wanted_down(self):
        import depth_ff2
        a = numpy.linspace(-4, 4, 161)
        for i0 in (-0.085, 0.088):
            X = numpy.stack([a, numpy.zeros_like(a), numpy.full_like(a, i0)], -1)
            total = depth_ff2.oracle(X) + i0 - depth_ff2.FF * a
            self.assertLessEqual(float(numpy.diff(total).max()), 1e-9)

    def test_bound(self):
        import depth_ff2
        X = depth_ff2.policy_samples(2000, numpy.random.default_rng(0))
        self.assertLessEqual(float(numpy.abs(depth_ff2.oracle(X, bound=0.18)).max()), 0.18)

    def test_gains_in_closed_loop_on_the_few_line_axis(self):
        import depth_ff2
        import depth_ff2_offline
        alone = depth_ff2_offline.closed_loop(None, 0)
        with_it = depth_ff2_offline.closed_loop(lambda X: depth_ff2.oracle(X, 16.8, bound=depth_ff2.BOUND), 0)
        self.assertLess(with_it["buoyancy -4 N"]["peak_cm"], 0.5 * alone["buoyancy -4 N"]["peak_cm"])
        self.assertLess(with_it["nominal 4 to 3 m"]["iae_m_s"], alone["nominal 4 to 3 m"]["iae_m_s"])


@unittest.skipUnless(HAVE_TORCH, "PyTorch is not installed")
class Model(unittest.TestCase):
    def test_the_plant_holds_no_trim_whatever_its_weights(self):
        import depth_ff2
        torch.manual_seed(5)
        plant = depth_ff2.make_plant()
        h = torch.linspace(-0.4, 0.4, 41)
        with torch.no_grad():
            still = plant(torch.stack([h, h, torch.zeros_like(h)], -1))
            moved = plant(torch.stack([h, h + 0.1, torch.full_like(h, 0.2)], -1))
        self.assertLess(float(still.abs().max()), 1e-6)
        self.assertGreater(float(moved.abs().max()), 1e-4)

    def test_the_learner_s_short_way_to_invert_is_the_long_one(self):
        import depth_ff2
        L = depth_ff2.Learner(seed=2)
        X = depth_ff2.policy_samples(200, numpy.random.default_rng(2))
        numpy.testing.assert_allclose(depth_ff2.invert(L, X), depth_ff2.invert(L.plant_np, X), atol=1e-6)

    def test_arch_is_the_profile_policy_s(self):
        import depth_ff2
        from hangar.nn import export_torch, hgw
        g = export_torch.to_graph(depth_ff2.make_model(), depth_ff2.INPUTS, ["heave"])
        self.assertEqual([op.type for op in g.ops],
                         ["affine", "dense", "tanh", "dense", "tanh", "dense", "tanh", "affine"])
        with open(os.path.join(PROFILE, "policies", "depth_ff.policy")) as f:
            arch = [ln.split("=")[1].strip() for ln in f if ln.startswith("Arch=")][0]
        self.assertEqual("%016x" % hgw.arch_hash(g), arch)

    def test_output_within_the_bound_whatever_the_weights(self):
        import depth_ff2
        torch.manual_seed(3)
        m = depth_ff2.make_model()
        with torch.no_grad():
            for layer in (m.l1, m.l2, m.l3):   # the trained weights; the two affines are fixed
                layer.weight.mul_(50.0)
                layer.bias.mul_(50.0)
            out = m(torch.randn(500, 3) * 10)
        self.assertLessEqual(float(out.abs().max()), depth_ff2.BOUND + 1e-6)

    def test_learns_the_deadband_s_side_and_leaves_the_trim_alone(self):
        import depth_ff2
        torch.set_num_threads(1)
        stream = depth_ff2.synthetic(256, minutes=2.0)
        L = depth_ff2.Learner(seed=0)
        for _ in range(1200):
            L.step(stream)
        L.settle(stream)
        L.model.eval()
        trim = float(numpy.median([r.y[depth_ff2.Y_REST] for r in stream.replay.rows]))
        self.assertLess(trim, -0.07)    # it floats: held pushing down, past the deadband
        with torch.no_grad():
            f = lambda a, i: float(L.model(torch.tensor([[a, 0.0, i]]))[0, 0])  # noqa: E731
            self.assertLess(abs(f(0.0, trim)), 0.03)       # still: the trim stays the integrator's
            self.assertGreater(f(-1.0, trim), 0.12)        # wants up: across the deadband
            self.assertLess(f(1.0, trim), -0.02)           # wants down: more of the same side,
            self.assertGreater(f(1.0, trim), -0.16)        # and less of it


if __name__ == "__main__":
    unittest.main()
