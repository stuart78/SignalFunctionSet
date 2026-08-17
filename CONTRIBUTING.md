# Contributing

Bug reports, feature ideas and questions are genuinely welcome. Code
contributions are not, and this file explains why, because "no pull requests"
without a reason is an unfriendly thing to run into.

## The short version

**Please don't send pull requests containing code.** Open an issue, or come to
[the Discord](https://discord.gg/zRRUez59dB), and describe the problem or the
idea. That is the most useful thing you can do, and it is the thing this project
actually needs.

## Why

Signal Function Set is dual licensed. The VCV Rack plugin is GPL-3.0 and always
will be. The same modules are also sold in other formats, currently as a Max for
Live pack, and that requires the maintainer to hold all the rights to the code.

Code written by someone else stays their copyright, licensed to this project
under the GPL, and cannot go into a commercial build without their agreement.
One merged patch would take the module it touched out of the paid pack until
that was sorted out. The alternative is to make everyone sign a contributor
agreement before their first patch, which is a lot of paperwork to put in front
of someone fixing a typo in a tooltip.

Given the choice, this project takes reports rather than patches. It is a
one-person project and the volume makes that workable.

## What helps most

- **Bug reports with a reproduction.** A patch file, the module settings, what
  you expected and what happened. Bugs in DSP are hard to find and easy to fix,
  so the finding is the valuable half.
- **Describing a fix in prose.** "The comb null lands on the 7th harmonic, so
  the string above 500 Hz loses its fundamental" is more useful than a diff, and
  carries no licensing weight at all.
- **Scala files, pattern banks and presets.** Data rather than code, and easy to
  include with attribution.
- **Documentation corrections**, which are welcome as pull requests.

## If a code contribution is accepted anyway

Occasionally something is worth merging as it stands. In that case you will be
asked to agree to the [Contributor License Agreement](CLA.md) first, which lets
the maintainer license your contribution commercially alongside the GPL release.
You keep your copyright. Nothing is merged before that agreement is recorded.

## Building

See the build instructions in [readme.md](readme.md), and `CLAUDE.md` for the
architecture and the conventions the modules follow.
