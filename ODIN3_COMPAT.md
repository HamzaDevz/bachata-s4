# Compatibilité Odin 3 : Bachata S4 (PS4)

Notre propre liste de compatibilité, tenue d'après nos tests sur l'AYN Odin 3 (Snapdragon 8 Elite, Adreno 830) avec notre build de la branche `odin3`. Chaque test met à jour la ligne du jeu : statut, mesure, date et commit testé.

Autres émulateurs : [Eden (Switch)](https://github.com/HamzaDevz/eden/blob/odin3/ODIN3_COMPAT.md) · [Azahar (3DS)](https://github.com/HamzaDevz/azahar/blob/odin3/ODIN3_COMPAT.md) · [Dolphin (GC/Wii)](https://github.com/HamzaDevz/dolphin/blob/odin3/ODIN3_COMPAT.md) · [Cemu (Wii U)](https://github.com/HamzaDevz/Cemu/blob/odin3/ODIN3_COMPAT.md) · [Vita3K (Vita)](https://github.com/HamzaDevz/Vita3K/blob/odin3/ODIN3_COMPAT.md) · [aPS3e (PS3)](https://github.com/HamzaDevz/aps3e/blob/odin3/ODIN3_COMPAT.md) · [Bachata S4 (PS4)](https://github.com/HamzaDevz/bachata-s4/blob/odin3/ODIN3_COMPAT.md) · [xemu (Xbox)](https://github.com/HamzaDevz/xemu/blob/odin3/ODIN3_COMPAT.md)

## Échelle

| Statut | Sens |
|---|---|
| 🟢 Parfait | vu en partie, plein régime, aucun défaut vu |
| 🔵 Jouable | vu en partie, défauts mineurs |
| 🟡 Problèmes | vu en partie, défauts gênants (fps, graphismes, son) |
| 🟠 Menus/intro | démarre, pas encore vu en partie |
| 🔴 Ne marche pas | crash, écran noir ou blocage |
| ⚪ Non testé | présent sur la console, jamais lancé |

## Comment on teste

Paquet `com.bachatas4.android.odin3` (« BachataS4 ») : l'APK 0.2.5 fermé, re-signé avec notre cœur `odin3` (shadPS4 + FEX), installé à côté de l'officiel `com.bachatas4.android.github`. Lancement par adb puis PLAY, Play Offline et Continue à la manette (`bb_pad.sh`) ; mesure sur 30 s par `tools/odin3/bb_measure.sh` (fps SurfaceFlinger, GPU, températures, puissance), sous le verrou partagé de la console. Patchs par jeu dans `files/patches/state/<CUSA>.json` (`bb_patches.sh`).

## Jeux

| Jeu | Statut | Vu jusqu'à | Mesure | Notes | Testé le | Build |
|---|---|---|---|---|---|---|
| Bloodborne GOTY v1.09 `CUSA03173` | 🔵 Jouable | création de personnage, clinique d'Iosefka, Rêve du chasseur | 30 fps stables en 720p, 11,2 W, CPU 104 °C, GPU 84 °C | réglages : patch 30 FPS++ en 1280x720, flou, aberration, SSAO, profondeur de champ, ombres dynamiques et bloom coupés, LOD -1 ; 60 fps hors de portée (37-40 fps en 720p, limité par le thread du jeu sous FEX ; 20-30 fps en 1080p) ; SoC à 104-107 °C ; tué une fois par lmkd au premier chargement (6,3-6,8 Go) | 2026-10-18 | 3e14ff9f |
