"""Retired synthetic-confinement compatibility tombstone.

Production confinement lives exclusively in :mod:`workspace_confinement` and
is minted internally by :func:`launch.authorize_launch`.  The former synthetic
proof classes, mint, validator, and usage-transport bridge were removed.  This
tracked tombstone is deliberately excluded from the installed harness inventory
so an adopting installation contains no legacy proof module.
"""

from __future__ import annotations

# Intentionally no API.  In particular this module must never define a
# ConfinementProof, prove_confinement, validate_proof, or synthetic mint.
__all__: tuple[str, ...] = ()
