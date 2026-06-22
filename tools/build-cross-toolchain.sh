#!/usr/bin/env bash
#
# Build i386-elf cross-compiler (binutils + gcc) para o StinkOS.
# Instala em ~/opt/cross. Roda uma vez. Leva ~30-60 min.
#
# Uso:
#   1) Instale os pre-requisitos (precisa sudo, faca a mao):
#        sudo apt update
#        sudo apt install build-essential bison flex texinfo \
#             libgmp3-dev libmpc-dev libmpfr-dev libisl-dev wget
#   2) bash tools/build-cross-toolchain.sh
#   3) Recarregue o shell:  source ~/.bashrc
#   4) Confira:  i386-elf-gcc --version
#
# Se as versoes abaixo derem 404, veja https://ftp.gnu.org/gnu/binutils/
# e https://ftp.gnu.org/gnu/gcc/ e ajuste BINUTILS_VER / GCC_VER.

set -euo pipefail

BINUTILS_VER="${BINUTILS_VER:-2.43}"
GCC_VER="${GCC_VER:-14.2.0}"

export PREFIX="${PREFIX:-$HOME/opt/cross}"
export TARGET="${TARGET:-i386-elf}"
export PATH="$PREFIX/bin:$PATH"

SRC="$HOME/src/stinkos-cross"
mkdir -p "$SRC"
cd "$SRC"

echo ">>> Alvo: $TARGET   Prefixo: $PREFIX"
echo ">>> binutils $BINUTILS_VER  /  gcc $GCC_VER"

# --- download ---
[ -f "binutils-$BINUTILS_VER.tar.gz" ] || \
  wget "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VER.tar.gz"
[ -f "gcc-$GCC_VER.tar.gz" ] || \
  wget "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.gz"

[ -d "binutils-$BINUTILS_VER" ] || tar xf "binutils-$BINUTILS_VER.tar.gz"
[ -d "gcc-$GCC_VER" ]           || tar xf "gcc-$GCC_VER.tar.gz"

# --- binutils ---
echo ">>> Compilando binutils..."
rm -rf build-binutils && mkdir build-binutils && cd build-binutils
../"binutils-$BINUTILS_VER"/configure \
  --target="$TARGET" --prefix="$PREFIX" \
  --with-sysroot --disable-nls --disable-werror
make -j"$(nproc)"
make install
cd "$SRC"

# confere que o gcc vai achar o assembler recem-instalado
command -v "$TARGET-as" >/dev/null || { echo "ERRO: $TARGET-as nao no PATH"; exit 1; }

# --- gcc (so compilador + libgcc, sem libc) ---
echo ">>> Compilando gcc (all-gcc + libgcc)..."
rm -rf build-gcc && mkdir build-gcc && cd build-gcc
../"gcc-$GCC_VER"/configure \
  --target="$TARGET" --prefix="$PREFIX" \
  --disable-nls --enable-languages=c,c++ --without-headers
make -j"$(nproc)" all-gcc
make -j"$(nproc)" all-target-libgcc
make install-gcc
make install-target-libgcc
cd "$SRC"

# --- PATH permanente ---
LINE='export PATH="$HOME/opt/cross/bin:$PATH"'
grep -qxF "$LINE" "$HOME/.bashrc" 2>/dev/null || echo "$LINE" >> "$HOME/.bashrc"

echo
echo ">>> PRONTO. Rode:  source ~/.bashrc"
echo ">>> Depois confira:"
echo "      i386-elf-gcc --version"
echo "      i386-elf-as  --version"
echo "      i386-elf-ld  --version"
