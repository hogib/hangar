# hangar: a lightweight module system for underwater vehicles, with a recorded learning loop

*Research proposal. H. Oğuz Bolat, Department of Geomatics Engineering, Yıldız Technical
University, İstanbul.*

<!-- Decide before sending: whether to add a note on the use of AI tools in preparing this
proposal and the literature survey behind it, and how to word it. -->

## Summary

hangar is a module system for remotely operated vehicles (ROVs) of the BlueROV2 class,
native to Linux and small enough for the vehicle's Raspberry Pi. Each function of the
vehicle is its own process under one manager; processes exchange fixed-size messages
through shared memory, and no reader can delay a writer. The learning loop is part of
this infrastructure, not an application on top of it. Every message records, by identifier,
the input messages it used. One dataset definition produces the same model inputs live,
offline and inside the running policy. Weights trained on a topside GPU are checked by
the vehicle before use, run in shadow until an operator promotes the evaluated model by
its hash, and are demoted automatically. Any thruster command can then be traced, by
identifiers alone, to the model that produced it, the rows that trained it, and the
sensor readings and pilot commands behind those rows. hangar supports known
deployment-time learning schemes (pilot takeovers as in HG-DAgger [1] and Sirius [2]; a
residual on a classical controller) and makes them recorded, checked and reversible on a
Pi. I propose to implement it in C11, compare it with ROS 2 on the same Pi, test its
failure behaviour on the bench, and measure what the loop costs and what its checks
catch.

## Motivation and problem

On every dive the pilot shows how the vehicle should be driven. Learning from this data
meets three obstacles.

A recording holds messages and timestamps, not which inputs a module used for each
output, so training pairs are rebuilt by matching times, and features built offline can
differ from what the model meets on the vehicle. Different code paths for training and
serving are the most common cause of this skew [3]: LeRobot trains on decoded video and
deploys on raw frames [4]; AquaBot trains on JPEG frames and deploys on uncompressed
ones, with a time join that can look forward in time [5]. Message-level provenance exists
for ROS, carried in headers [6] or reconstructed from traces [7], but it has not been
used to build datasets.

Moving a model trained topside onto a running vehicle is, in the systems we know of, a
manual step outside the middleware, with nothing on the vehicle to check the weights or
remove a misbehaving model.

ROS 2 brings a DDS participant per process and quadratic discovery to a computer that
also runs the autopilot and encodes video; its authors measure 2.1 to 4.3 MB per added
process and recommend composition into one process, which gives up isolation [8].

## Proposed approach

**Transport and supervision.** A topic is one shared-memory ring per output, with one
writer and any number of readers; a sequence counter per slot, read in Lamport's order
[9], detects torn copies. The ring itself is old; iceoryx2 is a current example [10].
What differs: readers hold no resources, so a dead or stuck reader needs no cleanup and
delays nobody, and schema, provenance and a staleness flag live in the ring file. The
manager restarts a failed module alone, with back-off, and moves no data; if it crashes
or is re-executed for an upgrade, the modules keep running and are adopted again from a
small registry. This is crash-only design [11] and one-for-one supervision [12]; the
recovery from a registry is ROS Rescue's, for the ROS 1 master [13].

**Provenance and datasets.** Each message lists up to four input messages as (topic,
sequence number), filled in by the library at publish time; an output computed from a
stale input is flagged, transitively. This is recorded usage, not proven derivation [14].
A dataset spec names an anchor topic and the inputs joined to it, by provenance or by a
bounded time join. The same join code runs in the live pairer on the vehicle, in the
offline build, and in the policy that builds its inputs. Live and offline rows are
identical except where a `late` flag marks a message that arrived after the live row was
emitted. A bridge mirrors topics over the tether with their sequence numbers, so a
provenance link means the same message on both sides, and pilot commands take priority
over dataset rows and weight files.

**Policies.** A policy module runs a small inference engine that allocates all memory at
load. The engine is standard engineering; the handover is not. A weights file carries an
architecture hash, probe inputs with expected outputs and tolerances, and the lineage of
its training rows. The vehicle checks hashes, finite values, probes and step time against
its tick budget, warms the model up beside the running one and swaps between two ticks;
`revert` restores the previous one. The probe check is differential testing in CRADLE's
sense [15], done by the receiving vehicle rather than offline.

**Safety.** All control sources pass through one multiplexer (the mux) that enforces
thrust, rate and depth limits per axis; the pilot wins on any axis. The mux is a
Simplex-style switch [16] with a hand-written envelope and a latched demotion that only a
human releases [17]; SOTER, the closest precedent, switches back automatically where
hangar waits for an operator [18]. A new model runs in shadow until promoted, and is
demoted if its output goes stale or sits at a limit for too long. One gate per actuator
outputs neutral unless the vehicle is armed and the command is fresh. The envelope bounds
commands, not states; it is not a safety proof. In phase 1 ArduSub still drives the
thrusters, held in MANUAL mode with its failsafes on, and hangar owns the ground-station
link. Version 1 of the loop drives only in residual mode, a bounded correction added to a
classical controller [19], as Neural-Fly did beside an autopilot on a Pi 4 [20].

## Contribution and prior work

A survey of about 110 works preceded this revision. Every part of hangar has prior art;
the claim is the combination and where it is done. To our knowledge, no system combines
the properties above, and none on an embedded vehicle behind a tether. hangar claims no
new learning method, safety method or loop; Table 1 names the closest prior work. The
secondary claims, a smaller footprint than ROS 2 with process isolation kept and safe
failure behaviour, are not new in kind and are reported as measured.

*Table 1. Closest prior work.*

| Work | What it has; what hangar adds, or merely combines |
|---|---|
| SERL and HIL-SERL [21, 22]; LeRobot [4] | The loop's shape is theirs (actor on the robot, learner on a GPU, weights pushed back while the robot runs, human takeover in the actor); hangar adds checks on received weights, shadow, promotion, demotion and lineage. |
| Sirius [2] | The scheme is Sirius's and HG-DAgger's [1] (per-sample "who acted" labels, retraining in rounds during deployment); hangar records the per-axis source in the data plane and makes rows, weights and promotions traceable. |
| Neural Simplex [23] | Shadow mode, retraining while the baseline drives and automatic switch-back, in simulation; hangar runs shadow on a real vehicle with an operator, not a test, in the switch. |
| Ford patents [24, 25] | A newly trained model run in shadow on the vehicle and validated against the previous one; a method, not a measured system, and as read without hash-bound operator promotion, probe or step-time checks, automatic demotion or lineage. |
| TFX [26], ML Metadata [27] | One transform for training and serving, "safe to serve" checks and artefact lineage, on servers; hangar moves the checks onto the vehicle and extends lineage from artefacts to messages and actuator commands. |
| Witte and Tichy [6]; Bédard et al. [7] | Message-level provenance in ROS, per field in headers or inferred from traces; hangar fills fixed-size identifier references at publish time, keeps them across the tether and builds datasets from them. |
| AquaBot [5] | Pilot demonstrations on a low-cost ROV, behaviour cloning and improvement from rollouts, with the policy topside on two GPUs; hangar runs the policy on the vehicle under a gate without AquaBot's train/serve skew, and is not the first to learn from an ROV pilot. |
| Carlucho et al. [28] | Online deep reinforcement learning on a real AUV with full authority to the network; hangar bounds that authority and records the loop, the supervisor layer they list as future work. |

## Evaluation plan

Table 2 lists eight measurements that no surveyed work reports and that carry the claim.
Each is a tool in the repository, reproducible by a script. Where the design states a
budget, that is the pass criterion; the rest are reported as measured.

*Table 2. Measurements that carry the claim.*

| Quantity | How it is measured |
|---|---|
| Train/serve mismatch rate, per dive | Each policy publishes (anchor sequence, CRC of its input) per tick; key-joined against the built rows; share of ticks that differ |
| Provenance overhead on a Pi | Publish latency and memory with 0 and with 4 references, Pi 4 and Pi 5 |
| Loop latency | Newest row a model trained on to its first promoted output, on one clock |
| Cost of a hot swap | Tick jitter and missed ticks before, during and after a weights load at 50 Hz |
| What the vehicle-side checks catch | Probe, tolerance and step-time refusals over mutated weights files (wrong operator semantics, precision change, corrupted weights) |
| Demotion and envelope-activation rates | Per dive and per model, from the mux state and the evaluation topic; false demotions listed |
| Lineage completeness | Share of thruster commands traceable to messages; forward queries from a bad message to the models it reached |
| Isolation at what cost | Memory (PSS) and latency of N isolated modules against composed ROS 2 [8] and iceoryx2 [10], same Pi 4, message sizes and rates |

Secondary: a fault campaign with propellers off (SIGKILL per module, 100 runs; time to
`ready`; time to neutral at the PWM output; manager crash and upgrade with calls in
flight); pilot overrides per promoted model; residual adaptation after a mass change, in
water; and the electric-eye port hashed against v0.3.1, with video latency no worse than
3.8 ms at 640×480 and 8.3 ms at 1280×720. The learning tasks run on recorded data, then
in simulation and on the bench, and in water only when a vehicle and a tank are
available.

## Work plan (draft, to be adjusted)

1. **Language and literature.** Done; see Status.
2. **Core.** Topics, manager, calls, parameters; budgets as tests; the benchmark on a Pi.
3. **Vehicle integration.** Safety, mux and gate; ArduSub in simulation; tether bridge;
   electric-eye supervised unchanged.
4. **Data.** Recorder, pairer, offline build; the live-equals-offline test.
5. **Learning loop.** Inference engine, weights file, lineage; both tasks on recorded data.
6. **Bench and water.** Fault campaign; both tasks on the bench, then in water if
   possible; electric-eye ported and hashed.
7. **Write-up.**

## Risks

- **Scope and hardware.** Fifteen design documents for one student; each phase ends in a
  measurable result, and the loop can be shown in simulation and on the bench without wet
  runs. The work needs a Pi 4 or 5, a topside computer with a GPU and, for wet runs, a
  vehicle and a tank.
- **The depth-hold example is wrong as designed.** It learns total heave and runs as a
  residual, so it would sit at its cap. Its label, the heave stick command, is not heave
  thrust: ArduSub's stock mixer scales all outputs down against the largest input to
  avoid saturation [29], so the command is not thrust once other axes are active. Label
  and cap are being reworked; the cap becomes a quantity measured on the vehicle.
- **Narrowed claims.** After the survey and two adversarial reviews, "byte-identical
  rows" became "identical except where flagged late"; "computed from" became "used";
  pilot imitation is HG-DAgger, not DAgger; the envelope gives no state guarantee; and
  version 1 drives only in residual mode. Three related works and the full claims of the
  Ford patents are still unread, so the claim may narrow further.
- **Weak policies and footprint.** A few dives may not train a good policy, and the
  survey found no precedent for a small network on a Pi CPU imitating a pilot from camera
  frames at 50 Hz. If hangar is not much lighter than composed ROS 2, that is reported.

## Current status

The design is complete in fifteen documents (00–14), including deployment (signed
releases, upgrade over the tether) and simulation (two managers on one machine, a 6-DOF
model, a camera fed from recorded footage, scripted faults), revised after two
adversarial reviews and a simplification pass. The language is decided: C11, static musl
binaries built with zig cc. A spike built the same topic and manager slice in C and in
Rust, on x86-64 only: the C manager is 50 KB static against 435 KB, idle resident memory
136 KB against 480 KB, publish 29 ns against 35 ns; wake latency is 5.4 µs at the median
for both, and both showed 0 torn reads over 250 million and 0 lost wake-ups. Nothing has
run on a Pi. electric-eye v0.3.1 [30], whose frame bus, process handling, discovery and
configuration hangar builds on, is forked into the repository and builds with zig cc
natively and as static AArch64 musl binaries with NEON; its 17 test suites pass natively
and under qemu-aarch64. OpenMP is unavailable in static musl builds, so its operator
loops will use a thread pool when ported. Licences: GPL-3.0-or-later for the system,
LGPL-3.0-or-later for the module library and the Python package. Nothing has run on a
vehicle.

## References

1. M. Kelly, C. Sidrane, K. Driggs-Campbell, M. J. Kochenderfer. HG-DAgger: Interactive Imitation Learning with Human Experts. *2019 International Conference on Robotics and Automation (ICRA)*, pp. 8077–8083, 2019. doi:10.1109/ICRA.2019.8793698. <!-- kelly2019hgdagger -->
2. H. Liu, S. Nasiriany, L. Zhang et al. Robot Learning on the Job: Human-in-the-Loop Autonomy and Learning During Deployment. *Robotics: Science and Systems XIX*, 2023. doi:10.15607/RSS.2023.XIX.005. <!-- liu2023sirius -->
3. E. Breck, N. Polyzotis, S. Roy et al. Data Validation for Machine Learning. *Proceedings of Machine Learning and Systems (MLSys/SysML 2019)*, 2019. <!-- breck2019datavalidation -->
4. R. Cadene, S. Aliberts, F. Capuano et al. LeRobot: An Open-Source Library for End-to-End Robot Learning. arXiv:2602.22818, 2026. <!-- cadene2026lerobot -->
5. R. Liu, H. Ha, M. Hou et al. Self-Improving Autonomous Underwater Manipulation. *2025 IEEE International Conference on Robotics and Automation (ICRA)*, pp. 16915–16922, 2025. doi:10.1109/ICRA55743.2025.11128759. <!-- liu2025aquabot -->
6. T. Witte, M. Tichy. Inferred Interactive Controls Through Provenance Tracking of ROS Message Data. *2021 IEEE/ACM 3rd International Workshop on Robotics Software Engineering (RoSE)*, pp. 67–74, 2021. doi:10.1109/RoSE52553.2021.00018. <!-- witte2021provenance -->
7. C. Bédard, P.-Y. Lajoie, G. Beltrame, M. Dagenais. Message Flow Analysis with Complex Causal Links for Distributed ROS 2 Systems. *Robotics and Autonomous Systems* 161:104361, 2023. doi:10.1016/j.robot.2022.104361. <!-- bedard2023messageflow -->
8. S. Macenski, A. Soragna, M. Carroll, Z. Ge. Impact of ROS 2 Node Composition in Robotic Systems. *IEEE Robotics and Automation Letters* 8(7):3996–4003, 2023. doi:10.1109/LRA.2023.3279614. <!-- macenski2023composition -->
9. L. Lamport. Concurrent Reading and Writing. *Communications of the ACM* 20(11):806–811, 1977. doi:10.1145/359863.359878. <!-- lamport1977concurrent -->
10. Eclipse iceoryx contributors. Eclipse iceoryx2: the true zero-copy data plane. GitHub repository, read at commit 69973dd, 2026. https://github.com/eclipse-iceoryx/iceoryx2 <!-- iceoryx2 -->
11. G. Candea, A. Fox. Crash-Only Software. *9th Workshop on Hot Topics in Operating Systems (HotOS IX)*, USENIX, 2003. <!-- candea2003crashonly -->
12. J. Armstrong. Making Reliable Distributed Systems in the Presence of Software Errors. PhD thesis, The Royal Institute of Technology (KTH), Stockholm, 2003. <!-- armstrong2003thesis -->
13. P. Kaveti, H. Singh. ROS Rescue: Fault Tolerance System for Robot Operating System. arXiv:1910.01078, 2019. <!-- kaveti2019rosrescue -->
14. L. Moreau, P. Missier (eds.). PROV-DM: The PROV Data Model. W3C Recommendation, World Wide Web Consortium, 2013. <!-- provdm2013 -->
15. H. V. Pham, T. Lutellier, W. Qi, L. Tan. CRADLE: Cross-Backend Validation to Detect and Localize Bugs in Deep Learning Libraries. *2019 IEEE/ACM 41st International Conference on Software Engineering (ICSE)*, pp. 1027–1038, 2019. doi:10.1109/ICSE.2019.00107. <!-- pham2019cradle -->
16. D. Seto, B. Krogh, L. Sha, A. Chutinan. The Simplex Architecture for Safe Online Control System Upgrades. *Proceedings of the 1998 American Control Conference (ACC)*, vol. 6, pp. 3504–3508, 1998. doi:10.1109/ACC.1998.703255. <!-- seto1998simplex -->
17. K. Hobbs, M. Mote, M. Abate et al. Run Time Assurance for Safety-Critical Systems: An Introduction to Safety Filtering Approaches for Complex Control Systems. *IEEE Control Systems* 43(2):28–65, 2023. doi:10.1109/MCS.2023.3234380. <!-- hobbs2023rta -->
18. A. Desai, S. Ghosh, S. A. Seshia et al. SOTER: A Runtime Assurance Framework for Programming Safe Robotics Systems. *2019 49th Annual IEEE/IFIP International Conference on Dependable Systems and Networks (DSN)*, pp. 138–150, 2019. doi:10.1109/DSN.2019.00027. <!-- desai2019soter -->
19. T. Johannink, S. Bahl, A. Nair et al. Residual Reinforcement Learning for Robot Control. *2019 International Conference on Robotics and Automation (ICRA)*, pp. 6023–6029, 2019. doi:10.1109/ICRA.2019.8794127. <!-- johannink2019residual -->
20. M. O'Connell, G. Shi, X. Shi et al. Neural-Fly Enables Rapid Learning for Agile Flight in Strong Winds. *Science Robotics* 7(66):eabm6597, 2022. doi:10.1126/scirobotics.abm6597. <!-- oconnell2022neuralfly -->
21. J. Luo, Z. Hu, C. Xu et al. SERL: A Software Suite for Sample-Efficient Robotic Reinforcement Learning. *2024 IEEE International Conference on Robotics and Automation (ICRA)*, pp. 16961–16969, 2024. doi:10.1109/ICRA57147.2024.10610040. <!-- luo2024serl -->
22. J. Luo, C. Xu, J. Wu, S. Levine. Precise and Dexterous Robotic Manipulation via Human-in-the-Loop Reinforcement Learning. *Science Robotics* 10(105):eads5033, 2025. doi:10.1126/scirobotics.ads5033. <!-- luo2025hilserl -->
23. D. T. Phan, R. Grosu, N. Jansen et al. Neural Simplex Architecture. *12th NASA Formal Methods Symposium (NFM 2020)*; arXiv:1908.00528, 2019. <!-- phan2020neuralsimplex -->
24. S. Singh, H. B. Bhate, Z. Rawashdeh et al. Automatic onboard validation of a newly trained vehicle machine learning model. US Patent 12,468,964 B2, Ford Global Technologies, LLC, 2025. <!-- ford2025onboardvalidation -->
25. U. Thakre, H. B. Bhate, S. Singh et al. Online in-vehicle learning for machine learning models. US Patent 12,561,586 B2, Ford Global Technologies, LLC, 2026. <!-- ford2026invehiclelearning -->
26. D. Baylor, E. Breck, H.-T. Cheng et al. TFX: A TensorFlow-Based Production-Scale Machine Learning Platform. *Proceedings of the 23rd ACM SIGKDD International Conference on Knowledge Discovery and Data Mining (KDD)*, pp. 1387–1395, 2017. doi:10.1145/3097983.3098021. <!-- baylor2017tfx -->
27. Google. ML Metadata (MLMD): For recording and retrieving metadata associated with ML developer and data scientist workflows. GitHub repository, documentation read 2026-11-07. https://github.com/google/ml-metadata <!-- mlmd -->
28. I. Carlucho, M. De Paula, S. Wang et al. Adaptive Low-Level Control of Autonomous Underwater Vehicles Using Deep Reinforcement Learning. *Robotics and Autonomous Systems* 107:71–86, 2018. doi:10.1016/j.robot.2018.05.016. <!-- carlucho2018adaptive -->
29. P. Ng, M. Krieg. Modifications to ArduSub That Improve BlueROV SITL Accuracy and Design of Hybrid Autopilot. *Applied Sciences* 14(17):7453, 2024. doi:10.3390/app14177453. <!-- ng2024ardusub -->
30. H. O. Bolat, H. Ersoy. electric-eye: Real-Time Image Enhancement and Low-Latency Video Transport for Remotely Operated Underwater Vehicles. Manuscript describing electric-eye v0.3.1. <!-- bolat_eeye -->
