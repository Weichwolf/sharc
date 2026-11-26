# SPDX-License-Identifier: GPL-3.0-only
"""Assertions and record assembly shared by independent expectation generators."""

from dataclasses import asdict


def require(actual, expected, context):
    if actual != expected:
        raise AssertionError(f"{context}: expected {expected}, got {actual}")


def require_raises(exception, call, message, *args, **kwargs):
    try:
        call(*args, **kwargs)
    except exception:
        return
    raise AssertionError(message)


def anchor(name, kind, actual, expected, **fields):
    require(actual, expected, name)
    return dict(name=name, kind=kind, **fields, expected=asdict(expected))
