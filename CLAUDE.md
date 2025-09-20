# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is a VCV Rack plugin called "Signal Function Set" that provides modular synthesizer modules. The plugin is currently in development and contains one module: Drift - a phase-shifted LFO with offset, attenuation, and stability controls featuring Lorenz attractor-based chaos functionality.

## Build Commands

- `make` - Build the plugin (creates plugin.dylib)
- `make clean` - Clean build artifacts 
- `make dist` - Create distribution package
- `RACK_DIR=.. make` - Build with explicit Rack SDK path (default is two directories up)
- `RACK_DIR=.. make clean` - Clean with explicit Rack SDK path
- `RACK_DIR=.. make dist` - Create distribution with explicit Rack SDK path

The build system uses the VCV Rack plugin framework via `$(RACK_DIR)/plugin.mk`.

## Code Architecture

### Plugin Structure
- `src/plugin.hpp` - Main plugin header with model declarations
- `src/plugin.cpp` - Plugin initialization and model registration
- `src/quadlfo.cpp` - Drift module implementation

### Module Development Pattern
Each module follows the VCV Rack module pattern:
- Inherits from `Module` class
- Defines `ParamId`, `InputId`, `OutputId`, and `LightId` enums
- Implements `process()` method for audio processing
- Has corresponding widget class for UI

### Drift Module Architecture
The Drift module implements:
- 4 phase-shifted LFO outputs (A, B, C, D)
- Lorenz attractor chaos system for each output
- Parameters: shape, stability, frequency, X spread, center, Y spread
- Inputs: CV control for all parameters
- Outputs: min/max values plus 4 LFO outputs

### Resources
- `res/` directory contains SVG panel designs
- `plugin.json` defines plugin metadata and module listings
- Panel SVGs are referenced by module widgets for UI rendering

## Development Notes

- The plugin uses VCV Rack SDK framework
- Module widgets are defined in the same file as the module implementation
- SVG panels in `res/` directory define the visual appearance
- Build artifacts go to `build/` and distribution files to `dist/`