# SPDX-License-Identifier: LGPL-3.0-or-later
"""hangar's Python side.

hangar.schema parses .msg schema texts: layout, canonical form, type hash,
parameter attributes, numpy dtypes. hangar.topic reads a live topic in
place, hangar.record a recording (.hgr), hangar.dataset a dataset spec and
its rows; hangar.learn is the trainer's side of the learning loop (rows in,
weights out) and hangar.nn the weights file, its reference and the PyTorch
exporter. Submodules are imported explicitly (`from hangar import schema`),
so that `python3 -m hangar.schema` runs without a second import of the same
module.
"""

__all__ = ["schema"]
