# SPDX-License-Identifier: LGPL-3.0-or-later
"""hangar's inference runtime, the Python side (docs/09-learning.md).

hgw          the .hgw weights file: a writer and a reader over plain Python
             lists, and the architecture hash lib/nn computes.
reference    a pure-Python, double-precision implementation of the runtime's
             ops, used to produce golden vectors for lib/nn/tests.
export_torch the PyTorch exporter (nn.Linear, nn.LSTM, activations, affine);
             imports without torch.

Submodules are imported explicitly: `from hangar.nn import hgw`.
"""

__all__ = ["hgw", "reference", "export_torch"]
