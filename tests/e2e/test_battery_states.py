"""E2E: real NUT status/values reported via `upsc`, for a running
sigennut driver against InvForge, across the battery states a NUT
client (upsmon etc.) actually needs to react to correctly.

Uses two of InvForge's real captured scenarios as ground truth for the
states they cover (OL, OB DISCHRG), and direct register overrides via
InvForge's /state endpoint for the states that don't have a dedicated
capture (LB, CHRG) -- deterministic and instant, no ramp-timing
dependency. Register addresses/encoding match
invforge/profiles/sigenergy/firmwares/V100R001C21SPC116/registers.py:
ess_soc (30014, U16, gain 10) and ess_power (30037, S32, gain 1000,
negative = discharging, positive = charging), both on the plant unit
(the profile's default unit, so no "<unit>:" prefix needed).
"""

from __future__ import annotations

from conftest import Stack


def test_idle_full_soc_reports_online(stack: Stack) -> None:
    stack.load_scenario("2026-08-14-idle-full-soc")
    out = stack.wait_for(lambda o: o.get("ups.status") == "OL")
    assert out["battery.charge"] == "100.0"
    assert out["input.frequency"] == "50.01"
    assert out["input.voltage"] == "240.4"


def test_discharge_reports_on_battery_discharging(stack: Stack) -> None:
    stack.load_scenario("2026-08-13-live-eps-discharge")
    out = stack.wait_for(lambda o: o.get("ups.status", "").startswith("OB"))
    assert "DISCHRG" in out["ups.status"]


def test_low_battery_flag(stack: Stack) -> None:
    stack.load_scenario("2026-08-14-idle-full-soc")
    stack.wait_for(lambda o: o.get("ups.status") == "OL")
    stack.set_state({"30014": 150})  # 15.0% SoC, below the 20% default LB threshold
    out = stack.wait_for(lambda o: "LB" in o.get("ups.status", ""))
    assert out["battery.charge"] == "15.0"
    assert "OL" in out["ups.status"]


def test_charging_flag(stack: Stack) -> None:
    stack.load_scenario("2026-08-14-idle-full-soc")
    stack.wait_for(lambda o: o.get("ups.status") == "OL")
    stack.set_state({"30037": [0, 2500]})  # +2.5kW ess_power = charging
    out = stack.wait_for(lambda o: "CHRG" in o.get("ups.status", ""))
    assert "OL" in out["ups.status"]


def test_discharge_and_low_battery_combine(stack: Stack) -> None:
    stack.load_scenario("2026-08-13-live-eps-discharge")
    stack.wait_for(lambda o: o.get("ups.status", "").startswith("OB"))
    stack.set_state({"30014": 150})
    out = stack.wait_for(lambda o: "LB" in o.get("ups.status", ""))
    assert "OB" in out["ups.status"]
    assert "DISCHRG" in out["ups.status"]
