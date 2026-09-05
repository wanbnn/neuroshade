#!/bin/sh

if [ "$#" -ne 2 ] || [ "$1" != "argument with spaces" ] || [ "$2" != "literal-*?" ]; then
    printf '%s\n' "wrapper-probe: arguments changed" >&2
    exit 90
fi

case ":${VK_INSTANCE_LAYERS-}:" in
    *:VK_LAYER_NEUROSHADE:*) ;;
    *) printf '%s\n' "wrapper-probe: layer not enabled" >&2; exit 91 ;;
esac

if [ "${NEUROSHADE_ENABLED-}" != "1" ] || [ "${NEUROSHADE_GAME_EXECUTABLE-}" != "/bin/sh" ]; then
    printf '%s\n' "wrapper-probe: runtime environment missing" >&2
    exit 92
fi

printf '%s\n' "wrapper-probe: PASS"
exit 23
