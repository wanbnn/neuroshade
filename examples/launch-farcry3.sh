#!/bin/sh
# launch-farcry3.sh — abre Far Cry 3 via Bottles com a camada Vulkan NeuroShade ativa.
#
# Fluxo:
#   neuroshade-run → exporta env vars NeuroShade
#                  → exec flatpak run --env=... bottles-cli run -b Games -e <bat>
#                  → Wine (soda-11.0-5, Unix 32 bits) carrega DXVK 3.0.2
#                  → DXVK injeta a camada NeuroShade (se tudo acima passar)
#                  → farcry3.exe inicia
#
# Por que --env= explícito pra tudo?
#   --env passa as variáveis ao Flatpak. O bottle Games também precisa listar
#   VK_LAYER_PATH, VK_INSTANCE_LAYERS e as NEUROSHADE_* usadas abaixo em
#   Inherited_Environment_Variables quando Limit_System_Environment=true.
#   Essa lista foi ajustada no bottle.yml; sem ela, Wine não recebe as vars.
#
# Por que NEUROSHADE_OVERLAY_VISIBLE=1?
#   Essa var força o overlay a abrir no startup,
#   permitindo confirmar que o render funciona. Para desabilitar e depender do
#   Home: exporte NEUROSHADE_OVERLAY_VISIBLE=0 antes de chamar o script.
#
# Debug:
#   O runtime escreve logs em ~/.local/state/neuroshade/fc3-runtime.log.
#   Rode `tail -f ~/.local/state/neuroshade/fc3-runtime.log` em outro terminal
#   enquanto joga pra ver mensagens tipo overlay=visible input=Home.

set -eu

# 1) CLI do NeuroShade no PATH
export PATH="$HOME/.local/bin:$PATH"

# 2) Caminhos
GAME_ROOT="${NEUROSHADE_GAME_ROOT:?Set NEUROSHADE_GAME_ROOT to the Far Cry 3 directory}"
BAT="$GAME_ROOT/1-FAR CRY 3 - NORMAL.bat"
# Far Cry 3 usa o loader Vulkan de 32 bits neste runner. A instalação padrão
# do NeuroShade é de 64 bits; este diretório contém a camada x86 dedicada.
export NEUROSHADE_LAYER_PATH="$HOME/.local/share/neuroshade/farcry3/layers"
NEUROSHADE_ROOT="$HOME/.local/share/neuroshade/farcry3/runtime"
NEUROSHADE_PROFILE_PATH="${NEUROSHADE_PROFILE:-$HOME/.config/neuroshade/farcry3.json}"
NS_LOG="$HOME/.local/state/neuroshade/fc3-runtime.log"
mkdir -p "$(dirname "$NS_LOG")"
: > "$NS_LOG"

# 3) Perfil NeuroShade registrado para farcry3.exe
#    (evita que neuroshade-run tente descobrir via hash de $1, que aqui é "flatpak")
export NEUROSHADE_PROFILE="$NEUROSHADE_PROFILE_PATH"
export NEUROSHADE_OVERLAY_VISIBLE="${NEUROSHADE_OVERLAY_VISIBLE:-1}"

# 4) Confere se a camada está instalada antes de tentar rodar
if [ ! -r "$NEUROSHADE_LAYER_PATH/VkLayer_neuroshade.json" ]; then
    printf '%s\n' "launch-farcry3.sh: camada NeuroShade não encontrada em $NEUROSHADE_LAYER_PATH" >&2
    printf '%s\n' "Restaure a camada x86 de NeuralShade/build/fc3-x86; a camada de 64 bits não serve para este runner." >&2
    exit 66
fi

# O servico nativo de 64 bits atende os modelos de jogos x86 via socket local.
# Sem o servico, os shaders continuam disponiveis; o painel informa falhas neurais.
if command -v neuroshade-runtime >/dev/null 2>&1; then
    if ! neuroshade-runtime start; then
        printf '%s\n' "NeuroShade: host64 indisponivel; consulte ~/.local/state/neuroshade/host64.log" >&2
    fi
fi

# 5) Verifica se o bottle Games existe
if ! flatpak run --command=bottles-cli com.usebottles.bottles list bottles 2>/dev/null | grep -q '^[[:space:]]*- Games$'; then
    printf '%s\n' "launch-farcry3.sh: bottle 'Games' não encontrado no Bottles" >&2
    exit 65
fi

# 6) Dispara. Todas as vars NeuroShade passam via --env= explícito pra evitar
#    filtragem do sandbox do flatpak.
# O runtime atual testa a presença da variável, inclusive quando vale "0".
OVERLAY_ARG="--env=NEUROSHADE_OVERLAY_VISIBLE=1"
if [ "$NEUROSHADE_OVERLAY_VISIBLE" = 0 ]; then
    OVERLAY_ARG="--unset-env=NEUROSHADE_OVERLAY_VISIBLE"
fi
exec neuroshade-run flatpak run \
    --env=VK_LAYER_PATH="$NEUROSHADE_LAYER_PATH" \
    --env=VK_INSTANCE_LAYERS=VK_LAYER_NEUROSHADE \
    --env=NEUROSHADE_ROOT="$NEUROSHADE_ROOT" \
    --env=NEUROSHADE_PROFILE="$NEUROSHADE_PROFILE_PATH" \
    --env=NEUROSHADE_LOG="$NS_LOG" \
    --env=NEUROSHADE_ENABLED=1 \
    "$OVERLAY_ARG" \
    --command=bottles-cli com.usebottles.bottles run \
    -b Games \
    -e "$BAT"
