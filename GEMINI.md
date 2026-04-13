# Engine Sim Exporter - Project Context

## Project Overview
**Engine Sim Exporter** is a specialized fork of the Engine Sim project, designed to function as an offline audio exporter for game engines (like Unity) and other audio pipelines. It simulates internal combustion engines based on script definitions (`.mr` files) and renders high-quality, loop-ready audio clips.

### Core Technologies
- **Language:** C++17
- **Build System:** CMake
- **Scripting:** Piranha (custom scripting language for engine definitions)
- **Parser Generation:** Flex & Bison
- **Testing:** GoogleTest
- **Audio Output:** 44.1 kHz, 16-bit, mono WAV files

### Architecture
1.  **`engine-sim` (Static Library):** Contains the core physics and simulation logic (thermodynamics, mechanics, acoustics).
2.  **`engine-sim-script-interpreter` (Static Library):** Bridges the Piranha scripting engine with the simulation core.
3.  **`engine-sim-exporter` (CLI Tool):** The primary entry point for this fork. It performs headless simulation and audio rendering.
4.  **`engine-sim-app` (Executable):** The original interactive GUI application (optional build).

---

## Building and Running

### Prerequisites
- CMake 3.10+
- C++17 compatible compiler (e.g., MSVC on Windows)
- Flex and Bison (must be in `PATH` or provided to CMake)

### Build Commands (Exporter Only)
```powershell
# Configure
cmake -S . -B build-exporter -DBUILD_APP=OFF -DDISCORD_ENABLED=OFF

# Build
cmake --build build-exporter --target engine-sim-exporter --config Release
```

### Running the Exporter
```powershell
.\build-exporter\Release\engine-sim-exporter.exe --out exports\unity_audio --duration 5 --throttle 30,70,100
```

### Running Tests
```powershell
# Build tests
cmake --build build-exporter --target engine-sim-test

# Run tests
.\build-exporter\Release\engine-sim-test.exe
```

---

## Development Conventions

### Coding Style
- **Naming:**
    - Classes: `PascalCase` (e.g., `CylinderBank`)
    - Methods/Variables: `camelCase` (e.g., `getDisplacement()`, `m_pistonArea`)
    - Macros/Constants: `SCREAMING_SNAKE_CASE`
- **Header Guards:** Follow the pattern `ATG_ENGINE_SIM_FILENAME_H`.
- **Physical Units:** Always use the `units` namespace helpers (found in `include/units.h`) for physical quantities like `units::rpm(6500)` or `units::pressure(1.0, units::atm)`.

### Testing Practices
- The project emphasizes physical correctness. Tests often verify conservation of energy and mass (see `test/gas_system_tests.cpp`).
- New simulation features should include corresponding unit tests in the `test/` directory.

### Scripting (`.mr` files)
- Engines are defined using a declarative scripting language.
- Key nodes include `engine`, `cylinder_bank`, `crankshaft`, `piston`, etc.
- Assets are located in the `assets/` directory, with engine definitions typically in `assets/engines/`.

---

## Key Directories
- `src/` & `include/`: Core simulation and application source code.
- `scripting/`: Implementation of the Piranha script interpreter.
- `assets/`: Engine scripts, sound samples, and themes.
- `test/`: Unit tests for various simulation components.
- `dependencies/`: External libraries managed as submodules.
