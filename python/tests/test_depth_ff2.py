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


def depth_ff2_inputs():
    import depth_ff2
    return list(depth_ff2.INPUTS)


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

    def test_a_row_without_its_command_says_nothing(self):
        # thr.out missing, or its window not yet back to the command LAG
        # ticks ago: the pulse widths are zeros, which read as full heave up
        # (+1.0). Such a row is no still row and no command the rows hold.
        import depth_ff2
        rp = learn.Replay(None)
        s = learn._Stream(depth_ff2._SynthCodec(), 4, None, rp, "t")
        t = depth_ff2.RestTracker()
        t.attach(s)
        for k in range(200):
            rp.add(row(1 + k, 0.0, 0.0, 0.0, -0.08))
        for k in range(100):
            r = learn._Row(300 + k, (300 + k) * 20_000_000, b"\x01" * 16, 0, [0.0, 0.0, 0.0],
                           [0.0, 0.0, 0.0, 0.0, 0.0, 7.0], False, partial=True)
            rp.add(r)
        self.assertAlmostEqual(r.y[depth_ff2.Y_REST], -0.08, places=6)
        self.assertEqual(t.applied(s), (-0.08, -0.08))

    @unittest.skipUnless(HAVE_NUMPY, "numpy is not installed")
    def test_the_rows_with_loss_are_the_stream_s(self):
        # The analysis (tools/sim/e2e_depth_ff2.py) and the trainer take the
        # same rows: depth_ff2.with_loss is learn's rule on the example's
        # fields. The command LAG ticks back is message WINDOW-1-LAG of the
        # window, there once the count reaches LAG + 1.
        import depth_ff2
        names = ["flags", "missing", "nav.state.vel_z", "ctl.depth_hold.target._seq", "ctl.depth_hold.target._count",
                 "ctl.depth_hold.target.accel_z", "ctl.depth_hold.target.integral", "made._seq", "made._count",
                 "made.accel", "thr.out._seq", "thr.out._count"] + ["thr.out.%d.pwm" % k for k in range(8)]
        rows = numpy.zeros(6, [(n, "<u4") for n in names])
        rows["ctl.depth_hold.target._count"] = rows["made._count"] = 1
        rows["thr.out._count"] = [8, depth_ff2.LAG + 1, depth_ff2.LAG, 0, 8, 8]
        rows["missing"] = [0, 0, 0, 4, 1, 0]
        rows["ctl.depth_hold.target._count"][4] = 0
        rows["flags"][5] = learn.MASKED
        self.assertEqual(list(depth_ff2.with_loss(rows)), [True, True, False, False, False, False])

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

    def test_another_vehicle(self):
        import depth_ff2
        # The payload: more to move and more drag, so more heave for the same
        # acceleration and to keep a velocity; nothing more with the vehicle still.
        X = numpy.array([[0.3, 0.0, -0.085], [0.0, 0.2, -0.085], [0.0, 0.0, -0.085]])
        bare, loaded = depth_ff2.oracle(X), depth_ff2.oracle(X, **depth_ff2.PAYLOAD)
        self.assertLess(float(loaded[0]), float(bare[0]) - 0.005)
        self.assertLess(float(loaded[1]), float(bare[1]) - 0.005)
        self.assertEqual(float(loaded[2]), 0.0)
        # The plant's side of it, and the parameters sim_vehicle is given: the
        # payload's weight is carried by the buoyancy, so the trim does not move.
        P = [[-0.2, -0.085, 0.2]]
        self.assertGreater(float(depth_ff2.sim_plant(P)[0]), float(depth_ff2.sim_plant(P, **depth_ff2.PAYLOAD)[0]))
        on = dict(x.split("=") for x in depth_ff2.payload_params(True))
        off = dict(x.split("=") for x in depth_ff2.payload_params(False))
        self.assertAlmostEqual(float(on["buoyancy"]) - float(off["buoyancy"]),
                               float(on["payload_mass"]) * depth_ff2.SIM_GRAVITY, places=3)
        self.assertEqual((float(off["payload_mass"]), float(off["buoyancy"])), (0.0, 114.8))
        # The dampings as msg/sim_vehicle_params.msg has them, but for the heave axis.
        with open(os.path.join(REPO, "msg", "sim_vehicle_params.msg")) as f:
            msg = {ln.split("=")[0].split()[1]: ln.split("=")[1].split("#")[0].split("[")[0] for ln in f
                   if ln.startswith("f32 d_") or ln.startswith("f32 buoyancy")}
        for name in ("d_lin[6]", "d_quad[6]"):
            want = [float(x) for x in msg[name].split(",")]
            self.assertEqual([float(x) for x in off[name[:-3]].split(",")], want)
            loaded = [float(x) for x in on[name[:-3]].split(",")]
            self.assertAlmostEqual(loaded[2], 5 * want[2])
            self.assertEqual(loaded[:2] + loaded[3:], want[:2] + want[3:])
        self.assertEqual(float(off["buoyancy"]), float(msg["buoyancy"]))

    def test_the_payload_leaves_a_later_model_something_to_gain(self):
        import depth_ff2_offline
        r = depth_ff2_offline.adaptation(1)
        both = {k: v[0]["both steps: IAE (m s)"] for k, v in r.items()}
        self.assertLess(both["the oracle of the vehicle without the payload"], 0.8 * both["the controller alone"])
        self.assertLess(both["the oracle of the vehicle with it"],
                        both["the oracle of the vehicle without the payload"] - 0.2)

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

    def test_the_oracle_is_exported(self):
        # --oracle: a model no rows trained, exported through learn.export
        # with a stand-in for the stream. The stand-in had fallen behind
        # export (lineage() without the session) and the command failed.
        import subprocess
        import tempfile
        from hangar.nn import hgw
        out = os.path.join(tempfile.mkdtemp(), "oracle.hgw")
        p = subprocess.run([sys.executable, os.path.join(REPO, "python", "examples", "depth_ff2.py"), "--oracle",
                            "--steps", "30", "--out", out], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
        self.assertEqual(p.returncode, 0, p.stdout.decode())
        f = hgw.read(out)
        self.assertEqual([b.field for b in f.graph.inputs], depth_ff2_inputs())
        self.assertEqual(f.header.lineage, [])
        self.assertGreaterEqual(f.probes[0].steps, 8)

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


@unittest.skipUnless(HAVE_TORCH, "PyTorch is not installed")
class AdaptationCriterion(unittest.TestCase):
    """tools/sim/e2e_depth_ff2.py's adapt_verdict: the criterion of
    bench/e2e-depth-ff/README.md, "Adaptation", on reports made up here."""

    @staticmethod
    def run_(seed, p1, p2, p3, variant="learned", **over):
        blk = lambda j: {"down": {"iae_m_s": 0.4 * j}, "up": {"iae_m_s": 0.6 * j}}  # noqa: E731
        r = {"variant": variant, "seed": seed, "blocks": {"P1": blk(p1), "P2": blk(p2), "P3": blk(p3)},
             "deploys": {"M1": {"hash": "aa"}, "M2": {"hash": "bb"}},
             "block_models": {"A2": ["aa"], "P1": ["aa"], "P3": ["aa"], "P2": ["bb"], "N2": ["bb"]},
             "residual_share_pct": {b: 100.0 for b in ("A2", "P1", "P2", "N2", "P3")},
             "demotions": {"demotions": []}}
        r.update(over)
        return r

    def runs(self, rows, variant="learned"):
        return [self.run_(k + 1, *row, variant=variant) for k, row in enumerate(rows)]

    def test_shown_not_shown_undecided(self):
        import e2e_depth_ff2 as e2e
        apart = [(4.7, 4.3, 4.7), (4.6, 4.3, 4.7), (4.7, 4.4, 4.6), (4.7, 4.3, 4.7), (4.6, 4.2, 4.7)]
        same = [(4.7, 4.7, 4.7), (4.6, 4.7, 4.7), (4.7, 4.6, 4.6), (4.7, 4.6, 4.7), (4.6, 4.6, 4.7)]
        oracle = self.runs(apart, "oracle")
        self.assertEqual(e2e.adapt_verdict(self.runs(apart), oracle)[0], "shown")
        self.assertEqual(e2e.adapt_verdict(self.runs(same), oracle)[0], "not shown")
        # Lower in every run, but one run's M2 is above another run's M1: the ranges overlap.
        over = [(4.7, 4.3, 4.7), (4.6, 4.3, 4.7), (4.7, 4.4, 4.6), (4.7, 4.3, 4.7), (4.95, 4.65, 4.9)]
        self.assertEqual(e2e.adapt_verdict(self.runs(over), oracle)[0], "not shown")
        # Lower than before, not than after, in one run.
        drift = [(4.7, 4.3, 4.7), (4.6, 4.3, 4.7), (4.7, 4.4, 4.6), (4.7, 4.3, 4.7), (4.7, 4.5, 4.45)]
        self.assertEqual(e2e.adapt_verdict(self.runs(drift), oracle)[0], "not shown")
        # The oracles themselves do not differ: nothing could have been seen.
        self.assertEqual(e2e.adapt_verdict(self.runs(apart), self.runs(same, "oracle"))[0], "undecided")
        self.assertEqual(e2e.adapt_verdict(self.runs(apart)[:4], oracle)[0], "undecided")

    def test_an_unfit_run_leaves_it_undecided(self):
        import e2e_depth_ff2 as e2e
        apart = [(4.7, 4.3, 4.7)] * 5
        oracle = self.runs(apart, "oracle")
        for over in ({"block_models": {"A2": ["aa"], "P1": ["aa"], "P3": ["bb"], "P2": ["bb"], "N2": ["bb"]}},
                     {"residual_share_pct": {"A2": 100.0, "P1": 100.0, "P2": 62.5, "N2": 100.0, "P3": 100.0}},
                     {"demotions": {"demotions": [{"t_s": 1.0}]}},
                     {"deploys": {"M1": {"hash": "aa"}, "M2": {"hash": "aa"}}}):
            rs = self.runs(apart)
            rs[2].update(over)
            verdict, why = e2e.adapt_verdict(rs, oracle)
            self.assertEqual(verdict, "undecided")
            self.assertTrue(any(x.startswith("Unfit: learned, seed 3") for x in why), why)


if __name__ == "__main__":
    unittest.main()
