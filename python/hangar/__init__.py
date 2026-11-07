# SPDX-License-Identifier: LGPL-3.0-or-later
"""hangar's Python side.

hangar.schema parses .msg schema texts: layout, canonical form, type hash,
parameter attributes, numpy dtypes. Recordings and datasets come later.
Submodules are imported explicitly (`from hangar import schema`), so that
`python3 -m hangar.schema` runs without a second import of the same module.
"""

__all__ = ["schema"]
