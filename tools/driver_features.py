"""Explicit opt-in switches for hardware validation candidates."""
import os

FEATURES = ("PS5_NATIVE_SUBMIT_COALESCE", "PS5_ASYNC_BATCH",
            "PS5_PRESENT_COMPLETION_WAIT", "PS5_LIGHT_DIAGNOSTICS",
            "PS5_CLFLUSHOPT")


def candidate_flags():
    flags = []
    for feature in FEATURES:
        value = os.environ.get(feature, "0")
        if value not in ("0", "1"):
            raise ValueError(f"{feature} must be 0 or 1, got {value!r}")
        flags.append(f"-D{feature}={value}")
    return flags
