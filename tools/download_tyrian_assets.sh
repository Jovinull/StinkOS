#!/bin/bash
# Download and extract Tyrian 2.1 data files for StinkOS OpenTyrian port

set -e

# Change to the script's directory
cd "$(dirname "$0")"
cd ..

# Ensure apps/tyrian/data exists
mkdir -p apps/tyrian/data

echo "Downloading Tyrian 2.1 freeware data files..."
# Download to a temporary location
wget -c "https://camanis.net/tyrian/tyrian21.zip" -O apps/tyrian/tyrian21.zip

echo "Extracting assets..."
# Unzip contents directly into data directory, ignoring paths, lowercase names
cd apps/tyrian/data
unzip -j -LL ../tyrian21.zip
cd ../../..

# Clean up
rm apps/tyrian/tyrian21.zip

echo "Tyrian data files downloaded and extracted to apps/tyrian/data successfully!"
