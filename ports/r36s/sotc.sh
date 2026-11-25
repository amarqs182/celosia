#!/bin/sh
# SotC launcher for R36S (RK3326 / Mali-G31 GLES 3.2 / SDL2 KMSDRM)
cd /roms/ports/sotc_data || exit 1
PORT_DIR="$(pwd)"

LOCKFILE="/tmp/sotc.lock"
if [ -e "$LOCKFILE" ]; then
  LOCK_PID="$(cat "$LOCKFILE" 2>/dev/null)"
  if [ -n "$LOCK_PID" ] && kill -0 "$LOCK_PID" 2>/dev/null; then
    echo "SotC ja esta em execucao (PID $LOCK_PID), abortando."
    exit 0
  fi
fi
echo "$$" > "$LOCKFILE"
STOPPED_ES=0
cleanup() {
  trap - INT TERM EXIT
  rm -f "$LOCKFILE"
  if [ "$STOPPED_ES" = "1" ]; then
    nohup /bin/bash /usr/bin/emulationstation/emulationstation.sh >/tmp/es_restart.log 2>&1 &
  fi
}
trap cleanup INT TERM EXIT

BOOT_ELF="./SCUS_974.72"
if [ ! -f "$BOOT_ELF" ] && [ -f "./NICO.ELF" ]; then
  cp -fp ./NICO.ELF ./SCUS_974.72 2>/dev/null
fi

if [ -n "${SSH_CONNECTION:-}" ] || [ -n "${SSH_TTY:-}" ]; then
  if pgrep -f emulationstation >/dev/null 2>&1; then
    STOPPED_ES=1
    pkill -f emulationstation 2>/dev/null
    pkill -f es-status-daemon 2>/dev/null
    sleep 1
  fi
fi
sudo systemctl stop batt_led.service 2>/dev/null

# Max frequencias CPU (1.5 GHz), GPU (520 MHz) e DMC/DRAM (666 MHz)
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor >/dev/null 2>&1
echo 1512000 | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_max_freq >/dev/null 2>&1
for df_dev in /sys/class/devfreq/*gpu* /sys/class/devfreq/*dmc*; do
  [ -d "$df_dev" ] || continue
  echo performance | sudo tee "$df_dev/governor" >/dev/null 2>&1
  DF_MAX_FREQ="$(cat "$df_dev/max_freq" 2>/dev/null)"
  [ -n "$DF_MAX_FREQ" ] && echo "$DF_MAX_FREQ" | sudo tee "$df_dev/min_freq" >/dev/null 2>&1
done

# Ativacao e configuracao do ZRAM
sudo swapon /dev/zram0 2>/dev/null
sudo sysctl -w vm.swappiness=60 >/dev/null 2>&1
echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null 2>&1

export SDL_GAMECONTROLLERCONFIG="1900bb3e4b4800000011000000010000,GO-Super Gamepad,a:b0,b:b1,back:b12,dpdown:b9,dpleft:b10,dpright:b11,dpup:b8,guide:b16,leftshoulder:b4,leftstick:b14,lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b15,righttrigger:b7,rightx:a2,righty:a3,start:b13,x:b2,y:b3,platform:Linux,"
unset PS2X_INPUT_SCRIPT PS2X_INPUT_CLOCK PS2X_INPUT_RECORD SOTC_INPUT_RECORD SOTC_INPUT_REPLAY PS2X_SNAPSHOT_DIR
export PS2X_MEMCARD_DIR="$PORT_DIR/memcards"
mkdir -p "$PORT_DIR/memcards" "$PORT_DIR/shader_cache" "$PORT_DIR/tex_cache" "$PORT_DIR/mesh_cache"
export SOTC_MALI_SHADER_CACHE_DIR="$PORT_DIR/shader_cache"
export SOTC_MALI_TEX_CACHE="$PORT_DIR/tex_cache"
export SOTC_MESH_CACHE=1
export SOTC_MESH_CACHE_DIR="$PORT_DIR/mesh_cache"
export SOTC_MESH_CACHE_MB=96

# 1. Renderizador Nativo OpenGL ES 3.2 na GPU Mali-G31 (640x480 & Audio Mono)
export SOTC_NATIVE=1
export SOTC_NATIVE_GL=1
export PS2X_SWAP_INTERVAL=0
export SOTC_FPS_CAP=30
export SOTC_CAM_INVERT_X=1
# Desabilitado em producao; para capturar microprogramas VU1 ausentes, defina:
# export PS2X_VU1_DUMP_MISSING="$PORT_DIR/vu1"
unset PS2X_VU1_DUMP_MISSING
export SOTC_FULLSCREEN=1
export SOTC_RES_W=640
export SOTC_RES_H=480
export SOTC_CROP_LETTERBOX=0
export SOTC_AUDIO_MONO=1
export PS2X_AUDIO_FRAMES=4096

# 2. Culling/Geometria do Jogo (Perfis Mali, Post-FX e Otimizacoes sao gerenciados via celosia_settings.cfg no submenu FN)
export SOTC_SAC_FAR_TERRAIN_SCALE=0
export SOTC_SAC_MIN_PROJECTED=0
export SOTC_SORT_OPAQUE=1
export SOTC_MESHOPT=1
export SOTC_MESHOPT_VCACHE=0
export SOTC_MESHOPT_OVERDRAW=0
export SOTC_DETAIL_TEX=1

# 3. Sincronizacao de Ciclos EE e Pacing Padrao
unset PS2X_NO_HOST_SYNC
export PS2X_HOST_CLOCK_MAX_LAG=2
unset SOTC_FIXED_DT
export SOTC_MAX_DT=0.0333334
unset SOTC_CAP20
export SOTC_FRAME_INTERVAL=1

# 4. Distribuicao Multi-Core e Afinidades de Threads
export SOTC_THREADS=1
# SOTC_THREADS=1 applies the fixed 4-core layout on Linux (threads::place)

# 5. Workers Paralelos e Decomp D1 Nativo
export SOTC_MDS_LANE=1
export SOTC_AI106=1
export SOTC_GRP106=1
export SOTC_WAIT143=1
export SOTC_JOIN95=1
export SOTC_TEX64=1
export SOTC_LEAN=1
export SOTC_LIFTD1=1
export SOTC_SORT140=1
export SOTC_FIN138=1
export SOTC_CLIP140=1
export SOTC_PF141=0
export SOTC_PFC141=0
export SOTC_PREPOSE=1
export SOTC_CLIP92=1
export SOTC_TAKEUP_MS=2
export SOTC_HOLD_TERRAIN_MS=0
export SOTC_COLL_CENSUS=0
export SOTC_COLLISION_STATS=0
export SOTC_COLL115_LOG=0
export SOTC_SEG123=0
export SOTC_RENDERER="${SOTC_RENDERER:-mali}"
# Todas as opcoes graficas, perfis (Desempenho Maximo / Medio / Original PS2/PS3) e flags Mali
# sao controladas pelo submenu FN (celosia_settings.cfg) com defaults nativos em defaultTuning().
export SOTC_GL_SHOTS=0
export SOTC_GL_SHOT_DIR=/tmp/shots
if [ "${SOTC_GL_SHOTS:-0}" != "0" ]; then
  mkdir -p /tmp/shots
fi  

# Executa diretamente sem overhead do GDB para performance maxima nativa
./ps2sotc_aarch64_pie_sdl "$BOOT_ELF" > /tmp/sotc.log 2>&1
