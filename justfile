#!/usr/bin/env just --justfile

# Using Just: https://github.com/casey/just?tab=readme-ov-file#installation

set quiet

# List all of the available commands.
default:
  just --list

# Configure the project using CMake presets (optionally specify MOQ_LOCAL path and preset)
setup path="" preset="":
	#!/usr/bin/env bash
	set -euo pipefail

	PRESET=$(just preset "{{preset}}")

	# Make .deps writable if it exists (fixes xattr permission issues)
	if [[ -d .deps ]]; then
		chmod -R u+w .deps
	fi

	# Add MOQ_LOCAL if path is provided
	if [[ -n "{{path}}" ]]; then
		echo "Configuring with preset: $PRESET and MOQ_LOCAL={{path}}"
		cmake --preset "$PRESET" -DMOQ_LOCAL="{{path}}"
	else
		echo "Configuring with preset: $PRESET"
		cmake --preset "$PRESET"
	fi

# Build the project using CMake presets (optionally specify preset name)
build preset="":
	#!/usr/bin/env bash
	set -euo pipefail

	PRESET=$(just preset "{{preset}}")

	cmake --build --preset "$PRESET"

# Run the CI checks
check:
	#!/usr/bin/env bash
	set -euo pipefail

	# Check C++ formatting
	find src -name '*.cpp' -o -name '*.h' | xargs clang-format --dry-run --Werror

# Automatically fix formatting issues.
fix:
	#!/usr/bin/env bash
	set -euo pipefail

	# Fix C++ formatting
	find src -name '*.cpp' -o -name '*.h' | xargs clang-format -i

# Detect the appropriate CMake preset for the current platform (or use override)
preset override="":
	#!/usr/bin/env bash
	set -euo pipefail

	# Use provided override or auto-detect
	if [[ -n "{{override}}" ]]; then
		echo "{{override}}"
	elif [[ "$OSTYPE" == "darwin"* ]]; then
		echo "macos"
	elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
		echo "ubuntu-x86_64"
	elif [[ "$OSTYPE" == "msys" || "$OSTYPE" == "cygwin" ]]; then
		echo "windows-x64"
	else
		echo "Unknown platform: $OSTYPE" >&2
		exit 1
	fi
