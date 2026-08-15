# Swell Manual

## Overview

Swell is a ping-driven envelope generator. Each rising edge on the PING input adds a configurable voltage rise to the current envelope value, then the envelope decays back toward zero. Multiple pings stack, so you can build up a slow swell from a stream of triggers, or get a single sharp attack from a single ping.

Where a typical AD/AR envelope produces one fixed-shape ramp per gate, Swell accumulates contributions from every trigger and bleeds them off continuously. The output is a single 0–10V CV that smoothly soft-saturates near the ceiling and is paused from decaying while a rise is still in flight.

The display shows a 1.2-second scope: the past trace on the left, the current voltage at center, and a projected future trace on the right that simulates forward from the current state using the live parameter values.

Swell is 6HP.

## Concepts

### Ping → Rise → Decay

A PING trigger queues a new **rise contribution**. Each rise adds the current Delta voltage over the current Rise time. Up to 32 rises can be in flight simultaneously, stacking additively. While any rise is still completing, decay is paused so the rise actually reaches its target. Once all rises are complete, the envelope decays back toward 0V over the Fall time.

### Soft Saturation

The output ceiling is 10V, but rather than hard-clipping, Swell scales each rise contribution by `(10 - V) / 10` as it approaches the ceiling. Pings that arrive while the envelope is already high contribute proportionally less, so V asymptotes smoothly toward 10V instead of slamming into a wall.

### Curve

The Curve control blends between two rate models, applied to both rises and decay:

- **Linear** (Curve = 0): rises add at a constant rate over the rise time, and decay subtracts at a constant rate over the fall time.
- **Exponential** (Curve = 1): rises add proportionally to remaining headroom (fast at first, slowing as the rise completes); decay subtracts proportionally to current voltage (a classic tau-based bleed).

In between, the two rates are mixed. Linear gives a tight AD-like shape; exponential gives a softer, more analog-feeling envelope.

## Controls

| Control | Range | Default | Function |
|---------|-------|---------|----------|
| **Delta** | 0–1 (0–10 V) | 0.5 (5 V) | Voltage added per ping. Tooltip displays in volts. |
| **Rise** | 1 ms – 2 s | ~10 ms | Time each rise takes to fully add its Delta. Exponential scaled. |
| **Fall** | 10 ms – 10 s | ~316 ms | Time the envelope takes to decay back to 0. Exponential scaled. |
| **Curve** | Linear → Exponential | 50% exp | Blends between linear and exponential rate models for both rise and fall. |

All four parameters accept ±5V CV with ±50% range, summed with the knob position and clamped to 0–1 internally.

## Inputs

| Input | Function |
|-------|----------|
| **PING** | Rising edge queues a new rise contribution at the current Delta and Rise time. |
| **RESET** | Rising edge zeroes the envelope and cancels all in-flight rises. |
| **Delta CV** | ±5V → ±50% of Delta param. |
| **Rise CV** | ±5V → ±50% of Rise param. |
| **Fall CV** | ±5V → ±50% of Fall param. |
| **Curve CV** | ±5V → ±50% of Curve param. |

## Outputs

| Output | Function |
|--------|----------|
| **CV** | 0–10V envelope, soft-saturated near the ceiling. |

## Display

The scope view shows a 1.2-second window: 600 ms of past samples drawn left of center, the current voltage at center (orange dot), and 600 ms of projected future drawn right of center (faint blue). The future trace is simulated forward using the current rise queue and decay parameters, so as you turn knobs, the projection updates immediately to show what the envelope will do next.

Gridlines mark 0 V, 5 V, and 10 V. The exact current voltage is displayed as text in the top-right corner.

## Patch Ideas

**Slow Swell from a Clock**: Patch a clock to PING, set Delta low (~0.1, 1V), Rise short (10 ms), Fall long (5–10 s), Curve high. Each tick adds a small bump; the envelope ramps up steadily and bleeds back down slowly. The faster the clock, the higher the steady-state voltage.

**Velocity-Sensitive Trigger**: Patch a trigger source to PING and an envelope or LFO to Delta CV. Each ping captures the current Delta: quiet pings produce small rises, loud pings produce bigger ones.

**Crescendo Detector**: Convert audio peaks to triggers (with a comparator) and feed them to PING. Sparse peaks produce a low envelope; dense peaks build up a swell that tracks intensity over time. Patch CV to a filter cutoff or VCA for an automatic crescendo follower.

**Multi-Tap Envelope**: Send the same trigger through several delays at different times into a multi-input mixer, then to PING. Each delayed copy fires its own rise; together they paint a custom multi-stage attack envelope.

**Smoothed Random Walk**: Patch a Bernoulli gate or random trigger source to PING, and modulate Delta CV with a slow LFO. The envelope drifts and swells unpredictably while still bleeding back toward 0 between bursts.

**RESET as Hard Mute**: With long Fall times, sometimes you want the envelope down NOW. Patch a manual button or end-of-phrase trigger to RESET to instantly zero the envelope and cancel any pending rises.
