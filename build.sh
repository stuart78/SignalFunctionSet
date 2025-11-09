#!/bin/bash

# VCV Rack Plugin Build Script
# Usage: ./build.sh [dev|prod]

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Check argument
if [ "$#" -ne 1 ]; then
    echo -e "${RED}Error: Missing build target${NC}"
    echo "Usage: $0 [dev|prod]"
    echo "  dev  - Development build with debug flags and -dev version suffix"
    echo "  prod - Production build optimized for release"
    exit 1
fi

BUILD_TARGET="$1"

if [[ "$BUILD_TARGET" != "dev" && "$BUILD_TARGET" != "prod" ]]; then
    echo -e "${RED}Error: Invalid build target '$BUILD_TARGET'${NC}"
    echo "Valid targets: dev, prod"
    exit 1
fi

# File paths
MAKEFILE="Makefile"
PLUGIN_JSON="plugin.json"
BACKUP_DIR=".build-backup"

echo -e "${GREEN}=== Building for ${BUILD_TARGET} ===${NC}\n"

# Create backup directory
mkdir -p "$BACKUP_DIR"

# Backup original files
echo "Backing up original files..."
cp "$MAKEFILE" "$BACKUP_DIR/Makefile.bak"
cp "$PLUGIN_JSON" "$BACKUP_DIR/plugin.json.bak"

# Function to restore files on exit
cleanup() {
    echo -e "\n${YELLOW}Restoring original files...${NC}"
    mv "$BACKUP_DIR/Makefile.bak" "$MAKEFILE"
    mv "$BACKUP_DIR/plugin.json.bak" "$PLUGIN_JSON"
    rmdir "$BACKUP_DIR" 2>/dev/null || true
    echo -e "${GREEN}Files restored${NC}"
}

# Set trap to restore files on exit (success or failure)
trap cleanup EXIT

# Modify Makefile based on target
echo "Configuring Makefile for $BUILD_TARGET build..."

if [ "$BUILD_TARGET" = "dev" ]; then
    # Comment out PROD section, uncomment DEV section
    sed -i.tmp '
        /^\[PROD-START\]/,/^\[PROD-END\]/ {
            /^\[PROD-START\]/n
            /^\[PROD-END\]/!s/^/# /
        }
        /^# \[DEV-START\]/,/^# \[DEV-END\]/ {
            s/^# \(FLAGS\)/\1/
            s/^# \(CFLAGS\)/\1/
            s/^# \(CXXFLAGS\)/\1/
            s/^# \(RACK_USER_DIR\)/\1/
            s/^# \(install:\)/\1/
            s/^# \([\t ]*mkdir\)/\1/
            s/^# \([\t ]*cp\)/\1/
        }
    ' "$MAKEFILE"
    rm -f "${MAKEFILE}.tmp"

    # Modify plugin.json for dev build
    echo "Configuring plugin.json for dev build..."

    # Add -dev suffix to slug (CRITICAL for coexistence) - only first occurrence
    sed -i.tmp '1,/^  "slug":/ s/"slug": *"\([^"]*\)"/"slug": "\1-dev"/' "$PLUGIN_JSON"

    # Add (Dev) to name - only first occurrence
    sed -i.tmp '1,/^  "name":/ s/"name": *"\([^"]*\)"/"name": "\1 (Dev)"/' "$PLUGIN_JSON"

    # Add -dev to brand
    sed -i.tmp 's/"brand": *"\([^"]*\)"/"brand": "\1-dev"/' "$PLUGIN_JSON"

    # Add -dev suffix to version
    sed -i.tmp 's/"version": *"\([^"]*\)"/"version": "\1-dev"/' "$PLUGIN_JSON"

    # Clean up any double -dev suffixes
    sed -i.tmp 's/-dev-dev/-dev/g' "$PLUGIN_JSON"
    sed -i.tmp 's/ (Dev) (Dev)/ (Dev)/g' "$PLUGIN_JSON"

    rm -f "${PLUGIN_JSON}.tmp"

elif [ "$BUILD_TARGET" = "prod" ]; then
    # PROD sections are already uncommented, DEV sections are already commented
    # Just ensure plugin.json doesn't have -dev modifications
    echo "Ensuring plugin.json has production configuration..."
    sed -i.tmp 's/"slug": *"\([^"]*\)-dev"/"slug": "\1"/g' "$PLUGIN_JSON"
    sed -i.tmp 's/"name": *"\([^"]*\) (Dev)"/"name": "\1"/g' "$PLUGIN_JSON"
    sed -i.tmp 's/"brand": *"\([^"]*\)-dev"/"brand": "\1"/g' "$PLUGIN_JSON"
    sed -i.tmp 's/"version": *"\([^"]*\)-dev"/"version": "\1"/g' "$PLUGIN_JSON"
    rm -f "${PLUGIN_JSON}.tmp"
fi

# Show what we're building
SLUG=$(grep '"slug"' "$PLUGIN_JSON" | head -1 | sed 's/.*"slug": *"\([^"]*\)".*/\1/')
NAME=$(grep '"name"' "$PLUGIN_JSON" | head -1 | sed 's/.*"name": *"\([^"]*\)".*/\1/')
VERSION=$(grep '"version"' "$PLUGIN_JSON" | sed 's/.*"version": *"\([^"]*\)".*/\1/')
echo -e "${GREEN}Building plugin:${NC}"
echo -e "  Slug: ${YELLOW}$SLUG${NC}"
echo -e "  Name: ${YELLOW}$NAME${NC}"
echo -e "  Version: ${YELLOW}$VERSION${NC}\n"

# Clean previous builds
echo "Cleaning previous builds..."
make clean

# Build the plugin
echo -e "\n${GREEN}Building plugin...${NC}"
make dist

# For dev builds, also install to Rack plugins folder
if [ "$BUILD_TARGET" = "dev" ]; then
    echo -e "\n${GREEN}Installing to VCV Rack plugins folder...${NC}"
    make install
    echo -e "${GREEN}Dev build installed!${NC}"
fi

echo -e "\n${GREEN}=== Build complete ===${NC}"
echo -e "Target: ${YELLOW}$BUILD_TARGET${NC}"
echo -e "Slug: ${YELLOW}$SLUG${NC}"
echo -e "Name: ${YELLOW}$NAME${NC}"
echo -e "Version: ${YELLOW}$VERSION${NC}"
echo -e "Output: ${YELLOW}dist/$SLUG-$VERSION-mac-arm64.vcvplugin${NC}"

if [ "$BUILD_TARGET" = "dev" ]; then
    echo -e "Installed to: ${YELLOW}~/Library/Application Support/Rack2/plugins-mac-arm64/${NC}"
fi

# Files will be restored automatically by the trap
