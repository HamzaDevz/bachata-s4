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
| Bloodborne GOTY v1.09 `CUSA03173` | 🔵 Jouable | création de personnage, clinique d'Iosefka, Rêve du chasseur | 30 fps en 720p (6,9 % d'images > 25 ms, pire 50 ms), 8,9 W batterie pleine, CPU 73 °C et GPU 53 °C après 3 min, cœur à 1,5 cœur CPU, 3,6 Go de RAM + 2,6 Go GPU | réglages : patch 30 FPS++ en 1280x720, flou, aberration, SSAO, profondeur de champ, ombres dynamiques et bloom coupés, LOD -1 ; le cœur ne tourne plus à vide (timers courts, attentes AJM, processeur de commandes GPU) et note CPU/RAM par fil toutes les 30 s (`BACHATA_USAGE` dans shadps4.log) pour repérer une zone qui s'emballe ; 60 fps hors de portée (37-40 fps en 720p, limité par le thread du jeu sous FEX ; 20-30 fps en 1080p) ; SoC à 104-107 °C en session longue sur 3e14ff9f, pas remesuré en session longue depuis ; tué une fois par lmkd au premier chargement (6,3-6,8 Go) | 2026-10-18 | 4be788c6 |
| Uncharted 4 v1.33 `CUSA00917` | 🟠 Menus/intro | écran de choix de la langue (pas encore validé à la manette) | 30 fps stables pendant 4 min sur ce menu, mais 6,8 cœurs CPU occupés (6 fils NdJobWorker à ~0,9 cœur chacun) et RAM du processus qui monte d'environ 160 Mo toutes les 30 s (3,3 → 4,6 Go en 4 min, 0,9 Go de RAM libre à la fin) | corrigés en route dans le cœur : libSceFiber en code invité, pointeurs opaques et petites structures dans les appels HLE, suivi des sharps et SRT dynamiques sur arm64, `ds_swizzle` en shuffle xor (cause du GPU perdu sur Turnip), toutes les comparaisons 64 bits, buffers à base non alignée sur 4, débordement de la pile call-ret de FEX par les fibers ; à faire : fuite mémoire et attente active des fils de jobs | 2026-10-18 | 88a2204c |
