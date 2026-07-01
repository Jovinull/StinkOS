#!/bin/bash
# test-userland.sh — bateria de testes da userland StinkOS
# Uso: bash tools/test-userland.sh
# Sai com código 0 se tudo OK, 1 se algum teste falhou.

cd "$(dirname "$0")/.."

PASS=0; FAIL=0; WARN=0
FAILURES=()

ok()      { echo "  [PASS] $*"; PASS=$((PASS+1)); }
fail()    { echo "  [FAIL] $*"; FAIL=$((FAIL+1)); FAILURES+=("$*"); }
warn()    { echo "  [WARN] $*"; WARN=$((WARN+1)); }
section() { echo; echo "── $* ────────────────────────────────────────────"; }
grep0()   { grep "$@" || true; }

# ── 1. Clean build ────────────────────────────────────────────────────────────
section "1. Clean build"
echo "  make clean && make all (capturando output)..."
BUILD_LOG=$(mktemp)
make clean > /dev/null 2>&1
if make all > "$BUILD_LOG" 2>&1; then
    ok "make all sucesso"
else
    fail "make all falhou"
    tail -10 "$BUILD_LOG"
fi

# Warnings não-banais (redefined/GNU-stack são esperados em cross-compile)
BAD_WARNS=$(grep "warning:" "$BUILD_LOG" | \
    grep -vE "redefined|GNU-stack|doom[0-9]*\.c" || true)
BAD_W_N=$(echo "$BAD_WARNS" | grep -c "warning:" 2>/dev/null || echo 0)
if [ "$BAD_W_N" -gt 0 ]; then
    warn "$BAD_W_N warning(s) inesperados:"
    echo "$BAD_WARNS" | head -5 | sed 's/^/    /'
else
    ok "zero warnings inesperados"
fi

# Warnings Rust
RUST_WARNS=$(grep "^warning" "$BUILD_LOG" | grep -vc "^warning: unused import\|^warning: dead_code" 2>/dev/null || echo 0)
if [ "$RUST_WARNS" -gt 0 ]; then
    warn "$RUST_WARNS warning(s) Rust"
    grep "^warning" "$BUILD_LOG" | head -3 | sed 's/^/    /'
else
    ok "zero warnings Rust inesperados"
fi
rm -f "$BUILD_LOG"

# ── 2. Headless boot ─────────────────────────────────────────────────────────
section "2. Boot headless (QEMU)"
echo "  make test-headless ..."
HEADLESS=$(make test-headless 2>&1)
if echo "$HEADLESS" | grep -q "^PASS:"; then
    ok "headless boot PASS"
    echo "  $(echo "$HEADLESS" | grep '^PASS:' | head -1)"
else
    fail "headless boot falhou"
    echo "$HEADLESS" | tail -5
fi

# ── 3. ELFs: existem, magic válido, tamanho ok ────────────────────────────────
section "3. ELF validity (51 binários userland)"
ELFS=($(ls build/*.elf 2>/dev/null | grep -v "kernel\|stripped" || true))

if [ "${#ELFS[@]}" -ge 51 ]; then
    ok "${#ELFS[@]} ELFs presentes"
else
    fail "apenas ${#ELFS[@]} ELFs, esperados >= 51"
fi

BAD_ELF=0; TINY_ELF=0
for elf in "${ELFS[@]}"; do
    name=$(basename "$elf")
    magic=$(xxd -l 4 "$elf" 2>/dev/null | awk 'NR==1{print $2$3}' | tr -d ' ')
    if [ "$magic" != "7f454c46" ]; then
        fail "$name: ELF magic inválido ($magic)"
        BAD_ELF=$((BAD_ELF+1)); continue
    fi
    sz=$(stat -c%s "$elf")
    if [ "$sz" -lt 256 ]; then
        fail "$name: muito pequeno ($sz bytes)"
        TINY_ELF=$((TINY_ELF+1))
    fi
done
[ $BAD_ELF -eq 0 ] && [ $TINY_ELF -eq 0 ] && ok "todos os ELFs têm magic ELF32 e tamanho ok"

# ── 4. Entry point: todos em 0x400000 ─────────────────────────────────────────
section "4. Entry point = 0x400000 (USER_CODE_BASE)"
# ELFs são stripped; entry point é a única evidência de linking correto
WRONG_EP=0
for elf in "${ELFS[@]}"; do
    name=$(basename "$elf")
    ep=$(readelf -h "$elf" 2>/dev/null | grep "Entry point" | awk '{print $NF}')
    if [ "$ep" != "0x400000" ]; then
        fail "$name: entry point errado ($ep, esperado 0x400000)"
        WRONG_EP=$((WRONG_EP+1))
    fi
done
[ $WRONG_EP -eq 0 ] && ok "todos os ELFs têm entry point 0x400000"

# ── 5. Seções de código não-vazias ─────────────────────────────────────────────
section "5. Seção .text não vazia"
EMPTY_TEXT=0
for elf in "${ELFS[@]}"; do
    name=$(basename "$elf")
    text_sz=$(readelf -S "$elf" 2>/dev/null | grep " .text " | awk '{print $7}' | head -1)
    if [ -z "$text_sz" ] || [ "$text_sz" = "000000" ]; then
        fail "$name: .text ausente ou vazia"
        EMPTY_TEXT=$((EMPTY_TEXT+1))
    fi
done
[ $EMPTY_TEXT -eq 0 ] && ok "todos os ELFs têm .text não vazia"

# ── 6. ELFs são EXEC i386 ─────────────────────────────────────────────────────
section "6. Tipo ELF: EXEC i386"
WRONG_TYPE=0
for elf in "${ELFS[@]}"; do
    name=$(basename "$elf")
    elf_type=$(readelf -h "$elf" 2>/dev/null | grep "^  Type:" | awk '{print $2}')
    elf_arch=$(readelf -h "$elf" 2>/dev/null | grep "Machine:" | awk '{print $NF}')
    if [ "$elf_type" != "EXEC" ]; then
        fail "$name: tipo $elf_type (esperado EXEC)"
        WRONG_TYPE=$((WRONG_TYPE+1))
    fi
    if [ "$elf_arch" != "Intel" ] && ! echo "$elf_arch" | grep -q "80386\|i386\|Intel"; then
        fail "$name: arch $elf_arch (esperado i386)"
        WRONG_TYPE=$((WRONG_TYPE+1))
    fi
done
[ $WRONG_TYPE -eq 0 ] && ok "todos os ELFs são EXEC i386"

# ── 7. StinkFS: conteúdo da imagem ────────────────────────────────────────────
section "7. StinkFS image"
FS_LINE=$(echo "$HEADLESS" | grep -o "wrote [0-9]* files" | head -1 || true)
if [ -n "$FS_LINE" ]; then
    FILES_N=$(echo "$FS_LINE" | awk '{print $2}')
    if [ "$FILES_N" -ge 50 ]; then
        ok "StinkFS: $FILES_N arquivos gravados"
    else
        fail "StinkFS: apenas $FILES_N arquivos (esperados >= 50)"
    fi
else
    warn "contagem StinkFS não encontrada no log"
fi

# ── 8. Panel apps: resize / F11 / poll_event ──────────────────────────────────
section "8. Panel apps: resize / F11 / poll_event"
PANEL=(
    "rs-about:apps/rust/rs-about/src/lib.rs"
    "rs-settings:apps/rust/rs-settings/src/lib.rs"
    "rs-clock:apps/rust/rs-clock/src/lib.rs"
    "rs-sysinfo:apps/rust/rs-sysinfo/src/lib.rs"
    "rs-taskman:apps/rust/rs-taskman/src/lib.rs"
    "rs-net:apps/rust/rs-net/src/lib.rs"
    "rs-calc:apps/rust/rs-calc/src/lib.rs"
)
for entry in "${PANEL[@]}"; do
    name="${entry%%:*}"; f="${entry##*:}"
    # strip null bytes (apps have embedded \x00 in byte literals)
    src=$(python3 -c "print(open('$f','rb').read().replace(b'\x00',b'').decode('utf-8','replace'))" 2>/dev/null)
    echo "$src" | grep -q "KEY_F11"        && ok "$name: F11"           || fail "$name: F11 ausente"
    echo "$src" | grep -q "win_poll_event"  && ok "$name: poll_event"   || fail "$name: poll_event ausente"
    echo "$src" | grep -q "WIN_EV_RESIZE"   && ok "$name: EV_RESIZE"    || fail "$name: EV_RESIZE ausente"
    echo "$src" | grep -q "let mut cw"      && ok "$name: cw/ch"        || fail "$name: cw/ch ausentes"
done

# ── 9. libui: APIs obrigatórias ───────────────────────────────────────────────
section "9. libui exports"
LIBUI="apps/rust/libui/src/lib.rs"
chk() { grep -q "$1" "$LIBUI" && ok "libui: $2" || fail "libui: $2 ausente"; }
chk "pub fn win_poll_event"  "win_poll_event()"
chk "pub fn win_resize"      "win_resize()"
chk "pub fn win_init_at"     "win_init_at()"
chk "pub fn win_init\b"      "win_init()"
chk "pub enum StinkError"    "StinkError enum"
chk "pub type StinkResult"   "StinkResult<T>"
chk "WIN_EV_RESIZE"          "WIN_EV_RESIZE const"
chk "WIN_EV_MOUSE"           "WIN_EV_MOUSE const"
chk "KEY_F11"                "KEY_F11 const"
chk "SCREEN_W_FULL"          "SCREEN_W_FULL const"
chk "pub fn fill"            "fill()"
chk "pub fn text16"          "text16()"
chk "pub fn window_frame"    "window_frame()"

# ── 10. Compositor: border resize ─────────────────────────────────────────────
section "10. Compositor: border resize (win.c)"
WIN_C="kernel/sys/win.c"
cw() { grep -q "$1" "$WIN_C" && ok "win.c: $2" || fail "win.c: $2 ausente"; }
cw "resize_slot"     "resize_slot state"
cw "RESIZE_BORDER"   "RESIZE_BORDER define"
cw "WIN_EV_RESIZE"   "WIN_EV_RESIZE push"
cw "resize_cur_w"    "resize_cur_w tracking"
cw "0x57f287"        "cor accent no preview"
cw "on_right"        "detecção borda direita"
cw "on_bottom"       "detecção borda baixo"
cw "btn1_up"         "btn1_up handler"

# ── 11. Syscall 94 stack completo ─────────────────────────────────────────────
section "11. Syscall 94 (win_resize)"
grep -q "case 94:"         kernel/sys/syscall.c    && ok "syscall.c: case 94"        || fail "syscall.c: case 94 ausente"
grep -q "sys_win_resize"   lib/libstink.h          && ok "libstink.h: declaração"    || fail "libstink.h: sys_win_resize ausente"
grep -q "sys_win_resize"   lib/libstink_syms.c     && ok "libstink_syms.c: símbolo"  || fail "libstink_syms.c: ausente"
grep -q "WIN_EV_RESIZE"    kernel/sys/win.h        && ok "win.h: WIN_EV_RESIZE"      || fail "win.h: WIN_EV_RESIZE ausente"
grep -q "int win_resize"   kernel/sys/win.h        && ok "win.h: win_resize decl"    || fail "win.h: win_resize decl ausente"

# ── 12. C apps: WIN_TITLEBAR_H ────────────────────────────────────────────────
section "12. C apps gráficos: WIN_TITLEBAR_H"
for app in anim asteroids breakout fbdemo pong snake; do
    src="apps/${app}.c"
    [ -f "$src" ] || { warn "$app.c não encontrado"; continue; }
    grep -q "WIN_TITLEBAR_H" "$src"       && ok "$app.c: WIN_TITLEBAR_H"       || fail "$app.c: sem WIN_TITLEBAR_H"
    if grep -qE "#define OY [[:space:]]*34[^0-9]" "$src"; then
        fail "$app.c: OY=34 hardcoded ainda presente"
    fi
done

# ── 13. rs-clock: vars no escopo correto ──────────────────────────────────────
section "13. rs-clock: vars em main()"
CLOCK_MAIN=$(python3 -c "
content = open('apps/rust/rs-clock/src/lib.rs','rb').read().replace(b'\x00',b'').decode('utf-8','replace')
idx = content.find('pub extern \"C\" fn main()')
print(content[idx:idx+500] if idx >= 0 else '')
" 2>/dev/null)
echo "$CLOCK_MAIN" | grep -q "let mut cw"         && ok "rs-clock: cw"          || fail "rs-clock: cw ausente em main()"
echo "$CLOCK_MAIN" | grep -q "let mut ch"         && ok "rs-clock: ch"          || fail "rs-clock: ch ausente em main()"
echo "$CLOCK_MAIN" | grep -q "let mut maximized"  && ok "rs-clock: maximized"   || fail "rs-clock: maximized ausente"
echo "$CLOCK_MAIN" | grep -q "let mut prev_k"     && ok "rs-clock: prev_k"      || fail "rs-clock: prev_k ausente"
echo "$CLOCK_MAIN" | grep -q "win_init_at"        && ok "rs-clock: win_init_at" || fail "rs-clock: win_init_at ausente"

# ── 14. Rust apps compilados: ELF maior que o mínimo esperado ─────────────────
section "14. Rust panel apps: tamanho ELF sanity"
RUST_MIN=4096   # mínimo 4 KB para apps gráficos Rust
for app in rs-about rs-settings rs-clock rs-sysinfo rs-taskman rs-net rs-calc; do
    elf="build/${app}.elf"
    if [ -f "$elf" ]; then
        sz=$(stat -c%s "$elf")
        if [ "$sz" -ge $RUST_MIN ]; then
            ok "$app.elf: ${sz}B (>= ${RUST_MIN}B)"
        else
            fail "$app.elf: muito pequeno (${sz}B < ${RUST_MIN}B)"
        fi
    else
        fail "$app.elf: não encontrado"
    fi
done

# ── Sumário ───────────────────────────────────────────────────────────────────
section "SUMÁRIO"
TOTAL=$((PASS+FAIL+WARN))
printf "  Total:   %d verificações\n" "$TOTAL"
printf "  PASS:    %d\n" "$PASS"
printf "  WARN:    %d\n" "$WARN"
printf "  FAIL:    %d\n" "$FAIL"
echo

if [ $FAIL -eq 0 ]; then
    echo "════════════════════════════════════════════════"
    echo "  RESULTADO: PASS — userland 100% ok"
    echo "════════════════════════════════════════════════"
    exit 0
else
    echo "════════════════════════════════════════════════"
    echo "  RESULTADO: FAIL — $FAIL problema(s):"
    for f in "${FAILURES[@]}"; do
        echo "    • $f"
    done
    echo "════════════════════════════════════════════════"
    exit 1
fi
