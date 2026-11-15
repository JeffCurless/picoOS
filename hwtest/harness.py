"""
harness.py — what test modules import: check(), requires(), Context
"""

from __future__ import annotations

from dataclasses import dataclass, field

from pico import Pico


class CheckFailed(AssertionError):
    pass


def check(cond: bool, what: str) -> None:
    """Fail the test with `what` unless cond holds.  (Not a bare assert:
    python -O would strip those.)"""
    if not cond:
        raise CheckFailed(what)


def requires(*features: str):
    """Mark a test as needing kit features, e.g. @requires("wifi", "display").
    Tests whose features the kit lacks are reported as SKIP."""
    def wrap(fn):
        fn.requires = set(features)
        return fn
    return wrap


@dataclass
class Context:
    pico: Pico
    kit: str | None            # path of the flashed kit, None with --no-flash
    version: str | None        # "0.4.8", parsed from the kit name
    board: str                 # pico | picow | pico2 | pico2w
    variant: str               # ND | D | D2
    features: set = field(default_factory=set)   # wifi, bt, display, led, slow
    notes: list = field(default_factory=list)

    def info(self, text: str) -> None:
        """Report text under this test's result line (and in the log)."""
        self.notes.append(text)
        self.pico.note(text)
