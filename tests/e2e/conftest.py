"""Fixtures for the e2e scenario/battery-state tests: a small client for
InvForge's HTTP control API and for querying the running sigennut driver
via `upsc`. Assumes the stack (docker-compose.yml in this directory) is
already up -- see .github/workflows/e2e.yml for how CI brings it up, or
run `docker compose -f tests/e2e/docker-compose.yml up -d --build`
locally first.
"""

from __future__ import annotations

import subprocess
import time
from collections.abc import Callable
from dataclasses import dataclass

import httpx
import pytest

CONTROL_URL = "http://127.0.0.1:8080"
UPSC_TARGET = "sigen@localhost"


@dataclass
class Stack:
    def load_scenario(self, name: str, speed: float = 1.0) -> None:
        r = httpx.post(f"{CONTROL_URL}/scenario", json={"name": name, "speed": speed}, timeout=10)
        r.raise_for_status()

    def set_state(self, registers: dict[str, int | list[int]]) -> None:
        r = httpx.post(f"{CONTROL_URL}/state", json={"registers": registers}, timeout=10)
        r.raise_for_status()

    def upsc(self) -> dict[str, str]:
        out = subprocess.run(
            ["upsc", UPSC_TARGET], capture_output=True, text=True, timeout=10, check=True
        )
        result: dict[str, str] = {}
        for line in out.stdout.splitlines():
            key, _, value = line.partition(": ")
            result[key] = value
        return result

    def wait_for(
        self, predicate: Callable[[dict[str, str]], bool], timeout: float = 15.0, interval: float = 1.0
    ) -> dict[str, str]:
        """Poll `upsc` (the driver's own 2s poll cycle plus propagation
        needs a moment) until predicate(output) is true, or fail with the
        last-seen output for a useful diff instead of a bare timeout."""
        deadline = time.monotonic() + timeout
        last: dict[str, str] = {}
        while time.monotonic() < deadline:
            try:
                last = self.upsc()
                if predicate(last):
                    return last
            except subprocess.CalledProcessError:
                pass
            time.sleep(interval)
        raise AssertionError(f"condition never met within {timeout}s; last upsc output: {last}")


@pytest.fixture(scope="session")
def stack() -> Stack:
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        try:
            httpx.get(f"{CONTROL_URL}/health", timeout=2).raise_for_status()
            return Stack()
        except httpx.HTTPError:
            time.sleep(1)
    raise RuntimeError("InvForge control API never came up")
