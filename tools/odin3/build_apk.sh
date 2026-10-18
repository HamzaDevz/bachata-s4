#!/usr/bin/env bash
# Rebuild the official Bachata S4 0.2.5 APK around our shadps4-arm64-fex core.
# usage: build_apk.sh <stripped core> <git short sha>
# The official APK (apk-dl/) and the output (out/) live in the untracked work tree BACHATA_WORK.
set -Eeuo pipefail
CORE="$1"; SHA="$2"
WORK="${BACHATA_WORK:-$HOME/PhpstormProjects/bachata-work}"
TOOLS="$(cd "$(dirname "$0")" && pwd)"
BT=~/Library/Android/sdk/build-tools/36.1.0
PKG=com.bachatas4.android.odin3
OUT="$WORK/out"; DEC="$OUT/decoded"
rm -rf "$DEC" && mkdir -p "$OUT"

apktool d -f -o "$DEC" "$WORK/apk-dl/bachatas4-0.2.5-release.apk" >/dev/null

# The app never keeps the screen on during play: a cutscene longer than the screen timeout dozes the
# device and the session is torn down a minute later. k03 is the game SurfaceView, so the flag lives
# exactly as long as the game is on screen.
perl -0pi -e 's#(    iput-wide p2, p0, Lk03;->d:J\n)#$1\n    const/4 p2, 0x1\n\n    invoke-virtual {p0, p2}, Landroid/view/View;->setKeepScreenOn(Z)V\n#' "$DEC/smali/k03.smali"
grep -q setKeepScreenOn "$DEC/smali/k03.smali"

# Distinct package and provider authorities, all-files access for SAF-resolved games.
sed -i '' \
    -e "s/com\.bachatas4\.android\.github\./$PKG./g" \
    -e "s/package=\"com\.bachatas4\.android\.github\"/package=\"$PKG\"/" \
    -e 's#<application #<application android:debuggable="true" #' \
    -e 's#<uses-permission android:name="android.permission.INTERNET"/>#<uses-permission android:name="android.permission.INTERNET"/><uses-permission android:name="android.permission.MANAGE_EXTERNAL_STORAGE"/>#' \
    "$DEC/AndroidManifest.xml"

python3 -I "$TOOLS/patch_runtime.py" "$DEC/assets/runtime" "$CORE" "runtime-odin3-$SHA"

apktool b -o "$OUT/unsigned.apk" "$DEC" >/dev/null
"$BT/zipalign" -f -P 16 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"
"$BT/apksigner" sign --ks ~/.android/debug.keystore --ks-pass pass:android \
    --ks-key-alias androiddebugkey --out "$OUT/bachatas4-0.2.5-odin3-$SHA.apk" "$OUT/aligned.apk"
rm -f "$OUT/unsigned.apk" "$OUT/aligned.apk"
echo "$OUT/bachatas4-0.2.5-odin3-$SHA.apk"
