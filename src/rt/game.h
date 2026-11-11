/* The game's names (runtime and installer) */
#pragma once
#define GAME_TITLE "Star Trek Generations"
#define GAME_SHORT "generations"
#define GAME_ID    "StarTrekGenerations"
#define GAME_ENV   "STG_"
/* files that identify the game's CDs (either disc) */
#define GAME_CD_FILES { "GENS\\COMMON.DAT", "GENS\\OPTIONS.DAT", "GENS\\STELLAR.DAT", NULL }
/* icon: taken from the game exe's own resources */
#define GAME_ICON_ICO NULL
#define GAME_ICON_LBM NULL
/* the game's own cursor position (two ints, 0..639 / 0..479) that it
   integrates from DirectInput mouse deltas: the port aims the deltas at the
   host pointer so both stay together */
#define GAME_MOUSE_XY 0x0053ff80u
