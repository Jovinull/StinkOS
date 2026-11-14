#!/usr/bin/env bash
#
# Build i386-elf cross-compiler (binutils + gcc) para o StinkOS.
# Instala em ~/opt/cross. Roda uma vez. Leva ~15-60 min (depende dos nucleos).
#
# Uso:
#   1) Pre-requisitos obrigatorios: gcc, g++, make, m4, tar, xz e wget ou curl.
#        Debian/Ubuntu:  sudo apt install build-essential m4 wget xz-utils
#        Fedora:         sudo dnf install gcc gcc-c++ make m4 wget xz
#   2) bash tools/build-cross-toolchain.sh
#   3) Recarregue o shell:  source ~/.bashrc
#   4) Confira:  i386-elf-gcc --version
#
# Opcionais (o script se vira sem eles, entao da pra rodar sem sudo):
#   - GMP / MPFR / MPC de desenvolvimento (libgmp3-dev libmpfr-dev libmpc-dev
#     no Debian; gmp-devel mpfr-devel libmpc-devel no Fedora). Se faltar
#     qualquer um, os tres sao baixados e compilados dentro da arvore do gcc
#     (contrib/download_prerequisites).
#   - texinfo. Sem makeinfo a documentacao .info simplesmente nao e gerada.
#   - bison / flex. Os tarballs de release ja trazem os parsers gerados.
#
# Variaveis de ambiente: BINUTILS_VER, GCC_VER, PREFIX, TARGET, SRC, CC, CXX.
#
# Se as versoes abaixo derem 404, veja https://ftp.gnu.org/gnu/binutils/
# e https://ftp.gnu.org/gnu/gcc/ e ajuste BINUTILS_VER / GCC_VER.

set -euo pipefail

BINUTILS_VER="${BINUTILS_VER:-2.43}"
GCC_VER="${GCC_VER:-14.2.0}"

DEFAULT_PREFIX="$HOME/opt/cross"
export PREFIX="${PREFIX:-$DEFAULT_PREFIX}"
export TARGET="${TARGET:-i386-elf}"
export PATH="$PREFIX/bin:$PATH"

# Compilador do host fixado em C17 / C++17. GCC 15+ usa C23 por padrao (e o
# GCC 16, C++20), dialetos que o configure do GMP e as fontes do GCC 14 nao
# esperam. Defina CC / CXX antes de chamar o script para escolher outro.
export CC="${CC:-gcc -std=gnu17}"
export CXX="${CXX:-g++ -std=gnu++17}"

SRC="${SRC:-$HOME/src/stinkos-cross}"
mkdir -p "$SRC"
cd "$SRC"

echo ">>> Alvo: $TARGET   Prefixo: $PREFIX"
echo ">>> binutils $BINUTILS_VER  /  gcc $GCC_VER"
echo ">>> Host: CC='$CC'  CXX='$CXX'"

# --- checagens do host ---
for tool in make m4 tar xz; do
  command -v "$tool" >/dev/null || { echo "ERRO: '$tool' nao encontrado no host"; exit 1; }
done

# fetch <url>: baixa para o diretorio atual, com wget ou curl.
fetch() {
  local url="$1" out="${1##*/}"
  if command -v wget >/dev/null; then
    wget -O "$out.part" "$url"
  elif command -v curl >/dev/null; then
    curl -fL --retry 3 -o "$out.part" "$url"
  else
    echo "ERRO: preciso de wget ou curl para baixar $url"; exit 1
  fi
  mv "$out.part" "$out"
}

# host_has_header <header>: o compilador do host enxerga esse header?
host_has_header() {
  echo "#include <$1>" | $CC -E -x c - >/dev/null 2>&1
}

# Sem makeinfo, MAKEINFO=true faz as regras de documentacao virarem no-op em
# vez de derrubar o build.
MAKEINFO_ARG=""
if ! command -v makeinfo >/dev/null; then
  echo ">>> makeinfo ausente: pulando a documentacao .info"
  MAKEINFO_ARG="MAKEINFO=true"
fi

# --- download ---
if [ ! -d "binutils-$BINUTILS_VER" ]; then
  [ -f "binutils-$BINUTILS_VER.tar.xz" ] || \
    fetch "https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VER.tar.xz"
  tar xf "binutils-$BINUTILS_VER.tar.xz"
fi
if [ ! -d "gcc-$GCC_VER" ]; then
  [ -f "gcc-$GCC_VER.tar.xz" ] || \
    fetch "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.xz"
  tar xf "gcc-$GCC_VER.tar.xz"
fi

# --- GMP / MPFR / MPC ---
# Usa os do sistema quando os tres headers existem; senao compila dentro da
# arvore do gcc. --no-graphite dispensa o isl (otimizacoes de loop Graphite,
# que o StinkOS nao usa).
if host_has_header gmp.h && host_has_header mpfr.h && host_has_header mpc.h; then
  echo ">>> GMP/MPFR/MPC: usando os do sistema"
else
  echo ">>> GMP/MPFR/MPC: ausentes no host, baixando para dentro da arvore do gcc"
  ( cd "gcc-$GCC_VER" && ./contrib/download_prerequisites --no-graphite )
fi

# --- binutils ---
echo ">>> Compilando binutils..."
rm -rf build-binutils && mkdir build-binutils && cd build-binutils
../"binutils-$BINUTILS_VER"/configure \
  --target="$TARGET" --prefix="$PREFIX" \
  --with-sysroot --disable-nls --disable-werror
make -j"$(nproc)" $MAKEINFO_ARG
make install $MAKEINFO_ARG
cd "$SRC"

# confere que o gcc vai achar o assembler recem-instalado
command -v "$TARGET-as" >/dev/null || { echo "ERRO: $TARGET-as nao no PATH"; exit 1; }

# --- gcc (so compilador + libgcc, sem libc) ---
echo ">>> Compilando gcc (all-gcc + libgcc)..."
rm -rf build-gcc && mkdir build-gcc && cd build-gcc
../"gcc-$GCC_VER"/configure \
  --target="$TARGET" --prefix="$PREFIX" \
  --disable-nls --enable-languages=c,c++ --without-headers
make -j"$(nproc)" all-gcc $MAKEINFO_ARG
make -j"$(nproc)" all-target-libgcc $MAKEINFO_ARG
make install-gcc $MAKEINFO_ARG
make install-target-libgcc $MAKEINFO_ARG
cd "$SRC"

# --- PATH permanente (so para o prefixo padrao) ---
if [ "$PREFIX" = "$DEFAULT_PREFIX" ]; then
  LINE='export PATH="$HOME/opt/cross/bin:$PATH"'
  grep -qxF "$LINE" "$HOME/.bashrc" 2>/dev/null || echo "$LINE" >> "$HOME/.bashrc"
  echo
  echo ">>> PRONTO. Rode:  source ~/.bashrc"
else
  echo
  echo ">>> PRONTO. Prefixo customizado: adicione $PREFIX/bin ao seu PATH."
fi
echo ">>> Depois confira:"
echo "      $TARGET-gcc --version"
echo "      $TARGET-as  --version"
echo "      $TARGET-ld  --version"
