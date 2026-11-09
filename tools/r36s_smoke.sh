#!/bin/bash
# r36s_smoke.sh - teste repetivel no R36S: jogo com dumps TARDIOS, sumario, restore ES + verificacao
# -----------------------------------------------------------------------------------------------
# Uso: ./tools/r36s_smoke.sh <tag> <outdir> [duracao] [envs...]
#   tag      : rotulo do teste (ex.: A, B, C, A2)
#   outdir   : diretorio de saida NO DEVICE (ex.: /tmp/mx/A) — game.log, summary.txt, ram.bin
#   duracao  : timeout em segundos (default 120); se o 3o arg nao for numero, e tratado como 120 e o arg vira env
#   envs...  : KEY=VALUE extras repassados ao `env` (ex.: SOTC_NATIVE=2)
#
# Defaults (config padrao medida — unica com IOP emulado confirmado, dropped=0):
#   SOTC_NATIVE=1 PS2X_NATIVE_IOP_BOOT=0 PS2X_NATIVE_IOP=0 PS2X_GS_NODRAW=0
#   PS2X_NO_SPU2_AUDIO=1 PS2X_STATS=1 PS2X_TRACE_PAD=1 PS2X_INPUT_CLOCK=frames
#   PS2X_TRACE_SYSCALLS OFF por default (0) — pode ser passado por envs, ex.: PS2X_TRACE_SYSCALLS=1
#   PS2X_INPUT_SCRIPT=<outdir>/input.txt + PS2X_DUMP_RAM_AT_SECONDS=35 PS2X_DUMP_RAM_PATH=<outdir>/ram.bin
#   (medido em /tmp/mx/ram.bin; aqui ram.bin vai para <outdir>/ram.bin)
#
# Configs suportadas via envs (sem editar o script):
#   Padrao : SOTC_NATIVE=1 (IOP emulado, dropped=0)
#   B      : SOTC_NATIVE=2  ex.: ./tools/r36s_smoke.sh B /tmp/mx/B 120 SOTC_NATIVE=2
#   C      : SOTC_NATIVE=0  ex.: ./tools/r36s_smoke.sh C /tmp/mx/C 120 SOTC_NATIVE=0
#   A2     : dump tardio t=70 com PS2X_DUMP_GSVRAM_PATH e PS2X_DUMP_GS_BUFFERS_AT
#            ex.: ./tools/r36s_smoke.sh A2 /tmp/mx/A2 120 PS2X_DUMP_RAM_AT_SECONDS=70 PS2X_DUMP_GSVRAM_PATH=/tmp/mx/A2/gsvram.bin PS2X_DUMP_GS_BUFFERS_AT=70
#
# Entrada (PS2X_INPUT_SCRIPT): 300v:START:12;+120v:START:12;+120v:START:12;+120v:START:12;+150v:CROSS:8;+150v:CROSS:8;+150v:CROSS:8;+150v:CROSS:8
#
# Binario no device: /roms/ports/SotC_Celosia/ps2sotc_aarch64_pie_sdl (md5 d9a17982dc91bcefb1beb895c27521b6, 51.350.952 B)
# Rodar como SCUS_974.72 (copia do NICO.ELF no mesmo dir).
#
# Acesso: WSL Ubuntu com sshpass; ark@192.168.2.121 senha ark. Usar SEMPRE arquivo + ssh 'bash -s' < arquivo.
#   sshpass -p ark ssh ark@192.168.2.121 'bash -s -- <tag> <outdir> [duracao] [envs...]' < tools/r36s_smoke.sh
#   sshpass -p ark ssh ark@192.168.2.121 'bash -s' < tools/r36s_smoke.sh
#
# REGRAS DO DEVICE (obrigatorias):
# - NUNCA ls /proc/[0-9]*/fd/* (congelou o R36S). Verificar fd card0 SOMENTE via PID especifico do ES
#   (ls -l /proc/<pid>/fd | grep card0), nunca varredura com glob.
# - NUNCA ffmpeg -f fbdev/kmsgrab (trava/congela display do R36S).
# - pkill SO dentro deste script atomico (inicio atomico + fim atomico). Nunca pkill ad-hoc fora do script.
# - sempre restaurar o ES e verificar (PID + fd card0 via PID do ES).
#
# Copiar log + dumps para o host (a partir do WSL, com outdir no device):
# scp -P 22 ark@192.168.2.121:/tmp/mx/<tag>/game.log ./<tag>_game.log
# scp -P 22 ark@192.168.2.121:/tmp/mx/<tag>/summary.txt ./<tag>_summary.txt
# scp -P 22 ark@192.168.2.121:/tmp/mx/<tag>/ram.bin ./<tag>_ram.bin
# scp -P 22 ark@192.168.2.121:/tmp/mx/<tag>/gsvram.bin ./<tag>_gsvram.bin
#-----------------------------------------------------------------------------------------------
set -u

TAG="${1:-}"
OUTDIR="${2:-}"

if [ -z "$TAG" ] || [ -z "$OUTDIR" ]; then
    echo "Uso: ./tools/r36s_smoke.sh <tag> <outdir> [duracao] [envs...]" >&2
    exit 2
fi

# Parse [duracao] [envs...]: se $3 for numero, e duracao; senao duracao default e $3... sao envs
DURATION=120
ENVS=()
if [ $# -ge 3 ]; then
    case "$3" in
        ""|*[!0-9]*)
            DURATION=120
            shift 2
            ENVS=("$@")
            ;;
        *)
            DURATION="$3"
            shift 3
            ENVS=("$@")
            ;;
    esac
fi

# Defaults (podem ser sobrepostos por envs do shell ou por KEY=VALUE em ENVS — ENVS vai depois no `env`)
SOTC_NATIVE="${SOTC_NATIVE:-1}"
PS2X_NATIVE_IOP_BOOT="${PS2X_NATIVE_IOP_BOOT:-0}"
PS2X_NATIVE_IOP="${PS2X_NATIVE_IOP:-0}"
PS2X_GS_NODRAW="${PS2X_GS_NODRAW:-0}"
PS2X_NO_SPU2_AUDIO="${PS2X_NO_SPU2_AUDIO:-1}"
PS2X_STATS="${PS2X_STATS:-1}"
PS2X_TRACE_PAD="${PS2X_TRACE_PAD:-1}"
PS2X_INPUT_CLOCK="${PS2X_INPUT_CLOCK:-frames}"
PS2X_TRACE_SYSCALLS="${PS2X_TRACE_SYSCALLS:-0}"
PS2X_DUMP_RAM_AT_SECONDS="${PS2X_DUMP_RAM_AT_SECONDS:-35}"

INPUT_SEQ='300v:START:12;+120v:START:12;+120v:START:12;+120v:START:12;+150v:CROSS:8;+150v:CROSS:8;+150v:CROSS:8;+150v:CROSS:8'

mkdir -p "$OUTDIR"
OUTDIR_ABS="$(cd "$OUTDIR" && pwd)"
GAME_LOG="$OUTDIR_ABS/game.log"
SUMMARY="$OUTDIR_ABS/summary.txt"
RAM_BIN="$OUTDIR_ABS/ram.bin"
INPUT_FILE="$OUTDIR_ABS/input.txt"

ES_RESTORED=0

restore_es() {
    if [ "$ES_RESTORED" -eq 1 ]; then
        return 0
    fi
    ES_RESTORED=1
    {
        echo "=== FIM ATOMICO: restore ES ==="
        pkill -f ps2sotc_aarch64_pie_sdl || true
        sleep 1
        nohup /bin/bash /usr/bin/emulationstation/emulationstation.sh >/tmp/es_restart.log 2>&1 &
        sleep 20
        echo "--- ES: ps aux | grep -i emulationstatio[n] ---"
        ps aux | grep -i emulationstatio[n] || echo "(sem saida de emulationstation no ps aux)"
        echo "--- ES: card0 via PID do ES ---"
        ES_PIDS="$(pgrep -f 'emulationstation/emulationstation' 2>&1 || true)"
        echo "pgrep -f 'emulationstation/emulationstation': $ES_PIDS"
        if [ -n "$ES_PIDS" ]; then
            for pid in $ES_PIDS; do
                echo "PID $pid fd card0:"
                ls -l /proc/$pid/fd 2>&1 | grep card0 || echo "(sem fd card0 para PID $pid)"
            done
            echo "ES restaurado e verificado"
            echo "# scp -P 22 ark@192.168.2.121:$GAME_LOG ./$(basename "$OUTDIR_ABS")_game.log"
            echo "# scp -P 22 ark@192.168.2.121:$SUMMARY ./$(basename "$OUTDIR_ABS")_summary.txt"
            echo "# scp -P 22 ark@192.168.2.121:$RAM_BIN ./$(basename "$OUTDIR_ABS")_ram.bin"
            echo "# scp -P 22 ark@192.168.2.121:$OUTDIR_ABS/gsvram.bin ./$(basename "$OUTDIR_ABS")_gsvram.bin"
        else
            echo "ES NÃO subiu"
        fi
        echo "--- limpeza (preservar /tmp/pad*) ---"
        ls -ld /tmp/pad* 2>&1 || echo "/tmp/pad* inexistente (preservado)"
        rm -f /tmp/r36s_smoke_tmp.sh /tmp/sotc_smoke_tmp.sh /tmp/ps2sotc_tmp.sh /tmp/r36s_input_tmp.txt
        echo "limpeza: rm -f /tmp/r36s_smoke_tmp.sh /tmp/sotc_smoke_tmp.sh /tmp/ps2sotc_tmp.sh /tmp/r36s_input_tmp.txt (scripts temporarios); /tmp/pad* preservado (nao removido)"
    } > "$OUTDIR_ABS/es_restore.log" 2>&1
    cat "$OUTDIR_ABS/es_restore.log" || true
    if grep -q "ES restaurado e verificado" "$OUTDIR_ABS/es_restore.log" 2>/dev/null; then
        echo "ES restaurado e verificado" >> "$SUMMARY" 2>/dev/null || true
    elif grep -q "ES NÃO subiu" "$OUTDIR_ABS/es_restore.log" 2>/dev/null; then
        echo "ES NÃO subiu" >> "$SUMMARY" 2>/dev/null || true
    fi
}

trap restore_es EXIT

printf '%s' "$INPUT_SEQ" > "$INPUT_FILE"

{
echo "=== R36S SMOKE TAG=$TAG OUTDIR=$OUTDIR_ABS DURATION=$DURATION ==="
echo "SOTC_NATIVE=$SOTC_NATIVE PS2X_NATIVE_IOP_BOOT=$PS2X_NATIVE_IOP_BOOT PS2X_NATIVE_IOP=$PS2X_NATIVE_IOP PS2X_GS_NODRAW=$PS2X_GS_NODRAW PS2X_NO_SPU2_AUDIO=$PS2X_NO_SPU2_AUDIO PS2X_STATS=$PS2X_STATS PS2X_TRACE_PAD=$PS2X_TRACE_PAD PS2X_INPUT_CLOCK=$PS2X_INPUT_CLOCK PS2X_TRACE_SYSCALLS=$PS2X_TRACE_SYSCALLS PS2X_DUMP_RAM_AT_SECONDS=$PS2X_DUMP_RAM_AT_SECONDS"
echo "ENVS: ${ENVS[*]:-}"
} > "$SUMMARY" 2>&1

pkill -f ps2sotc_aarch64_pie_sdl || true
sleep 1
pkill -f emulationstation || true
sleep 1

{
echo "--- card0: fuser /dev/dri/card0 ---"
if fuser /dev/dri/card0 2>&1; then
    echo "card0 em uso (fuser acima)"
else
    echo "CARD0_FREE"
fi
} >> "$SUMMARY" 2>&1

cd /roms/ports/SotC_Celosia
cp -f NICO.ELF SCUS_974.72
{
echo "--- binario ---"
ls -lh /roms/ports/SotC_Celosia/ps2sotc_aarch64_pie_sdl
md5sum /roms/ports/SotC_Celosia/ps2sotc_aarch64_pie_sdl
} >> "$SUMMARY" 2>&1

timeout "$DURATION" env SOTC_NATIVE="$SOTC_NATIVE" PS2X_NATIVE_IOP_BOOT="$PS2X_NATIVE_IOP_BOOT" PS2X_NATIVE_IOP="$PS2X_NATIVE_IOP" PS2X_GS_NODRAW="$PS2X_GS_NODRAW" PS2X_NO_SPU2_AUDIO="$PS2X_NO_SPU2_AUDIO" PS2X_STATS="$PS2X_STATS" PS2X_TRACE_PAD="$PS2X_TRACE_PAD" PS2X_INPUT_CLOCK="$PS2X_INPUT_CLOCK" PS2X_TRACE_SYSCALLS="$PS2X_TRACE_SYSCALLS" PS2X_DUMP_RAM_AT_SECONDS="$PS2X_DUMP_RAM_AT_SECONDS" PS2X_DUMP_RAM_PATH="$RAM_BIN" PS2X_INPUT_SCRIPT="$INPUT_FILE" "${ENVS[@]}" ./ps2sotc_aarch64_pie_sdl > "$GAME_LOG" 2>&1 || true

{
echo "=== SUMMARY TAG=$TAG OUTDIR=$OUTDIR_ABS DURATION=$DURATION ==="
echo "--- [stats] ---"
grep '\[stats\]' "$GAME_LOG" | tail -n 20 || echo "(sem [stats])"
echo "--- input buttons= ---"
grep 'buttons=' "$GAME_LOG" | tail -n 20 || echo "(sem buttons=)"
echo "--- sceCd ---"
grep 'sceCd' "$GAME_LOG" | tail -n 20 || echo "(sem sceCd)"
echo "--- mscal=[^0] ---"
grep -E 'mscal=[^0]' "$GAME_LOG" | tail -n 20 || echo "(sem mscal!=0)"
echo "--- vu1Instr=[^0] ---"
grep -E 'vu1Instr=[^0]' "$GAME_LOG" | tail -n 20 || echo "(sem vu1Instr!=0)"
echo "--- dropped ---"
grep 'dropped' "$GAME_LOG" | tail -n 20 || echo "(sem dropped)"
echo "--- [ee:sema] signal ---"
grep -E '\[ee:sema\] signal' "$GAME_LOG" | tail -n 20 || echo "(sem [ee:sema] signal)"
echo "--- Unknown syscallId ---"
grep 'Unknown syscallId' "$GAME_LOG" | tail -n 20 || echo "(sem Unknown syscallId)"
echo "--- [dump] ---"
grep '\[dump\]' "$GAME_LOG" | tail -n 20 || echo "(sem [dump])"
echo "--- ram.bin ---"
ls -lh "$RAM_BIN" 2>&1 || echo "(sem $RAM_BIN)"
ls -lh "$OUTDIR_ABS"/ram.bin "$OUTDIR_ABS"/gsvram.bin 2>&1 || true
echo "--- prims (tri,ln,sprite) ---"
echo "tri_total=$(grep -c 'tri' "$GAME_LOG" 2>/dev/null || echo 0)"
echo "ln_total=$(grep -c 'ln' "$GAME_LOG" 2>/dev/null || echo 0)"
echo "sprite_total=$(grep -c 'sprite' "$GAME_LOG" 2>/dev/null || echo 0)"
echo "tri_eq=$(grep -c 'tri=' "$GAME_LOG" 2>/dev/null || echo 0)"
echo "ln_eq=$(grep -c 'ln=' "$GAME_LOG" 2>/dev/null || echo 0)"
echo "sprite_eq=$(grep -c 'sprite=' "$GAME_LOG" 2>/dev/null || echo 0)"
echo "--- copiar para o host (WSL, comentado) ---"
echo "# scp -P 22 ark@192.168.2.121:$GAME_LOG ./$(basename "$OUTDIR_ABS")_game.log"
echo "# scp -P 22 ark@192.168.2.121:$SUMMARY ./$(basename "$OUTDIR_ABS")_summary.txt"
echo "# scp -P 22 ark@192.168.2.121:$RAM_BIN ./$(basename "$OUTDIR_ABS")_ram.bin"
} >> "$SUMMARY" 2>&1

restore_es

echo "done TAG=$TAG OUTDIR=$OUTDIR_ABS"
