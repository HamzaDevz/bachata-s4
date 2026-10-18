#!/bin/zsh
# bb_patches.sh id… : remplace les patchs activés de Bloodborne (CUSA03173) dans l'appli arrêtée.
# L'état réel est files/patches/state/<serial>.json ; la session est régénérée depuis lui à chaque lancement.
A=~/Library/Android/sdk/platform-tools/adb; PKG=${PKG:-com.bachatas4.android.odin3}
J=$(mktemp)
python3 -I - "$@" > $J <<'PY'
import json, sys
print(json.dumps({"disabled_patch_ids": [], "enabled_patch_ids": sys.argv[1:], "last_seen_repository_revision": "",
                  "repository_id": "bachata-official", "schema": 1, "selected_preset": None, "serial": "CUSA03173",
                  "updated_at": ""}, indent=2))
PY
$A shell am force-stop $PKG
$A push $J /data/local/tmp/bb_state.json >/dev/null && rm $J
$A shell "run-as $PKG sh -c 'cat > files/patches/state/CUSA03173.json' < /data/local/tmp/bb_state.json && rm /data/local/tmp/bb_state.json"
