#!/bin/sh
# SotC launcher for R36S (RK3326 / Mali-G31 GLES 3.2 / SDL2 KMSDRM)
cd "$(dirname "$0")" || exit 1
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
trap 'rm -f "$LOCKFILE"; nohup /bin/bash /usr/bin/emulationstation/emulationstation.sh >/tmp/es_restart.log 2>&1 &' INT TERM EXIT

BOOT_ELF="./SCUS_974.72"
if [ ! -f "$BOOT_ELF" ] && [ -f "./NICO.ELF" ]; then
  cp -fp ./NICO.ELF ./SCUS_974.72 2>/dev/null
fi

pkill -f emulationstation 2>/dev/null
pkill -f es-status-daemon 2>/dev/null
sudo systemctl stop batt_led.service 2>/dev/null
sleep 1

# Max frequencias CPU (1.5 GHz) e GPU (520 MHz max suportado pelo kernel)
echo performance | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor >/dev/null 2>&1
echo 1512000 | sudo tee /sys/devices/system/cpu/cpu*/cpufreq/scaling_max_freq >/dev/null 2>&1
echo performance | sudo tee /sys/class/devfreq/*gpu*/governor >/dev/null 2>&1
echo 600000000 | sudo tee /sys/class/devfreq/*gpu*/min_freq >/dev/null 2>&1

# Ativacao e configuracao do ZRAM
sudo swapon /dev/zram0 2>/dev/null
sudo sysctl -w vm.swappiness=60 >/dev/null 2>&1
echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null 2>&1

export SDL_GAMECONTROLLERCONFIG="1900bb3e4b4800000011000000010000,GO-Super Gamepad,a:b0,b:b1,back:b12,dpdown:b9,dpleft:b10,dpright:b11,dpup:b8,guide:b16,leftshoulder:b4,leftstick:b14,lefttrigger:b6,leftx:a0,lefty:a1,rightshoulder:b5,rightstick:b15,righttrigger:b7,rightx:a2,righty:a3,start:b13,x:b2,y:b3,platform:Linux,"
unset PS2X_INPUT_SCRIPT PS2X_INPUT_CLOCK PS2X_INPUT_RECORD SOTC_INPUT_RECORD SOTC_INPUT_REPLAY PS2X_SNAPSHOT_DIR
export PS2X_MEMCARD_DIR="$PORT_DIR/memcards"
mkdir -p "$PORT_DIR/memcards"

# 1. Renderizador Nativo OpenGL ES 3.2 na GPU Mali-G31 (Resolucao Nativa 100% 640x480 & Audio Mono)
export SOTC_NATIVE=1
export SOTC_NATIVE_GL=1
export SOTC_RENDER_SCALE=1.0
export SOTC_TEXTURE_FILTER=1
export PS2X_SWAP_INTERVAL=0
export SOTC_FPS_CAP=60
export SOTC_CAM_INVERT_X=1
export PS2X_VU1_DUMP_MISSING="$PORT_DIR/vu1"
export SOTC_FULLSCREEN=1
export SOTC_RES_W=640
export SOTC_RES_H=480
export SOTC_STRETCH=1
export SOTC_SCREEN_MODE=1
export SOTC_CROP_LETTERBOX=0
export SOTC_AUDIO_MONO=1

# 2. Desligamento TOTAL dos Efeitos Pesados Multipass do PS2
export SOTC_BLOOM_SCALE=0
export PS2X_AUDIO_FRAMES=4096
export SOTC_GLOW=0
export SOTC_NATIVE_NOGLOW=1
export SOTC_NATIVE_NOFEEDBACK=1
export SOTC_NATIVE_NOSBLUR=1
export SOTC_NATIVE_NODOF=1
export SOTC_NATIVE_NOHAZE=1
export SOTC_NATIVE_NOSHADOWS=1
export SOTC_BODY_SHADOWS=0
export SOTC_NATIVE_NOREFLECT=1
export SOTC_NATIVE_NOREFRACT=1
export SOTC_NATIVE_NOPTCL=1
export SOTC_SAC_PARTICLES=1
export SOTC_NATIVE_NOVFOG=1
export SOTC_NATIVE_ZFOG=0
export SOTC_FOG_AMOUNT=1
export SOTC_ALPHA_TEST=1
export SOTC_NATIVE_NOADDITIVE=0

# 3. Geometria Estavel Sem Pop-in, Culling Nativo de Frustum e Meshopt
export SOTC_PRESET=1
export SOTC_SACRIFICES=1
export SOTC_SAC_LOD=1
export SOTC_SAC_LOD_SCALE=1.6
export SOTC_SAC_DRAWDIST=1
export SOTC_SAC_DRAWDIST_SCALE=0.75
export SOTC_SAC_FAR_TERRAIN=1
export SOTC_SAC_FAR_TERRAIN_SCALE=0
export SOTC_SAC_MIN_PROJECTED=0
export SOTC_SAC_CAMCOLL=1
export SOTC_SAC_FARCHARA=1
export SOTC_SAC_FARCHARA_DIST=35.0
export SOTC_SAC_FX=1
export SOTC_GL_SORT87=1
export SOTC_SORT_OPAQUE=1
export SOTC_GL_TEXARRAY=1
export SOTC_GL_PROMOTE_128=1
export SOTC_GL_STATIC_VAO=1
export SOTC_GL_DRAW_RANGE=1
export SOTC_GL_SORT_PROG=1
export SOTC_MESHOPT=1
export SOTC_MESHOPT_VCACHE=0
export SOTC_MESHOPT_OVERDRAW=0
export SOTC_DETAIL_TEX=1

# 4. Sincronizacao de Ciclos EE e Pacing Padrao (estilo Remake)
unset PS2X_NO_HOST_SYNC
export PS2X_HOST_CLOCK_MAX_LAG=2
unset SOTC_FIXED_DT
export SOTC_MAX_DT=0.0333334
unset SOTC_CAP20
export SOTC_CAP30=0
export SOTC_FRAME_INTERVAL=1

# Round 119: Fast contact set & cache for collision walks and finder lists
export SOTC_WALK119=1
export SOTC_VIEW119=1
export SOTC_PART119=1
export SOTC_FLUSH119=1

# 5. Geometria Otimizada, Batching e Fast Paths do Renderizador GL
export SOTC_GL_INSTANCING=1
export SOTC_GL_MERGE=1
export SOTC_GL_POOLS=1
export SOTC_GL_LEAN=1
export SOTC_GL_LEAN_DRAW=1
export SOTC_GL_LEAN_PASS=1
export SOTC_GL_PLAIN_READS=1
export SOTC_GL_PRESENT86=1
export SOTC_GL_ATOP93=1
export SOTC_GL_LOD0_98=1
export SOTC_GL_BGCOPY94=0
export SOTC_GL_NOFOG98=1
export SOTC_GL_UNTEX98=1
export SOTC_GL_PSOFT98=0
export SOTC_GL_AT98=0
export SOTC_GL_FAT116=1
export SOTC_GL_MERGE_STATS=0
export SOTC_GL_PHASE_LOG=0
export SOTC_GL_DRAW_LOG=0
export SOTC_PREPOSE=1
export SOTC_CLIP92=1

# 6. Distribuicao Multi-Core e Afinidades de Threads
export SOTC_THREADS=1
export SOTC_AFF_GAME=0
export SOTC_AFF_RENDER=3
export SOTC_AFF_HOOK=1
export SOTC_AFF_AUDIO=2
export SOTC_AFF_VU1=1
export SOTC_AFF_DMA=2
export SOTC_AFF_SCHED=0
export SOTC_AFF_MDSLANE=2

# 7. Workers Paralelos e Decomp D1 Nativo
export SOTC_CHAR104=0
export SOTC_MDS_LANE=1
export SOTC_LANE_HELP=3
export SOTC_AI106=1
export SOTC_GRP106=1
export SOTC_WAIT143=1
export SOTC_JOIN95=1
export SOTC_PTCL91=1
export SOTC_TEX64=1
export SOTC_MDSLANE_CPU=2
export SOTC_LEAN=1
export SOTC_EE_TRUST=1
export SOTC_LIFTD1=1
export SOTC_SORT140=1
export SOTC_FIN138=1
export SOTC_POSE136=1
export SOTC_CLIP140=1
export SOTC_PF141=0
export SOTC_PFC141=0
export SOTC_RENDER_THREAD=1
export SOTC_GL_TRIPLE_BUF=1
export SOTC_HOOK_ASYNC=1
export SOTC_ASYNC_BUILDS=1
export SOTC_HOLD_INCOMPLETE=1
export SOTC_HOLD_TERRAIN_MS=300
export SOTC_GL_WARM=1
export SOTC_GL_NODISCARD=0
export SOTC_GL_DEPTH_KEEP=1
export SOTC_GL_FEW_COPIES=1
export SOTC_GL_FEW_SCENES=1
export SOTC_GL_OPAQUE88=0
export SOTC_GL_BGSMALL87=1
export SOTC_GL_MIRROR88=1
export SOTC_GPU_SKIN=1

export SOTC_FRAME_LOG=0
export SOTC_COLL_CENSUS=0
export SOTC_COLLISION_STATS=0
export SOTC_COLL115_LOG=0
export SOTC_SEG123=1
export SOTC_GL_SHOTS=0
export SOTC_GL_SHOT_DIR=/tmp/shots
mkdir -p /tmp/shots

# Executa diretamente sem overhead do GDB para performance maxima nativa
./ps2sotc_aarch64_pie_sdl "$BOOT_ELF" > /tmp/sotc.log 2>&1
