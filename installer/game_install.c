/* What the installer does that is particular to Star Trek Generations.
 *
 * The game comes on two CDs; each has a GENS folder with its share of the
 * missions and movies.  Both are copied to <install>\game\cd (the game's D:
 * drive), disc 2's GENS merged into disc 1's, so no disc is ever asked for.
 * <install>\game\hd is C:\STGENS, what the original InstallShield setup put
 * on the hard disk: sgens.exe and sgens.ini from SUPP.Z on disc 1 (the game
 * code is in our exe; its data and resources are read from sgens.exe), plus
 * the empty SAVEGAME and WORLD folders. */
#include "setup.h"
#include <string.h>
#include "install.h"

static int has_all(CdSrc *c, const char *const *files)
{
    if (!c) return 0;
    for (int i = 0; files[i]; i++)
        if (!cd_exists(c, files[i])) { setup_log("CD check: %s missing", files[i]); return 0; }
    return 1;
}

int game_is_cd(CdSrc *c)
{
    static const char *const files[] = GAME_CD_FILES;
    return has_all(c, files);
}

int game_is_cd2(CdSrc *c)
{
    static const char *const files[] = GAME_CD2_FILES;
    return has_all(c, files);
}

int game_install_files(const char *cd, const char *hd)
{
    char p[MAX_PATH * 2];
    snprintf(p, sizeof p, "%s\\SUPP.Z", cd);
    static const char *const want[] = {"sgens.exe", "sgens.ini", NULL};
    if (is3_extract(p, hd, want) != 2) {
        MessageBoxA(NULL, "Unpacking the game program from SUPP.Z on disc 1 failed.", GAME_TITLE " setup", MB_ICONERROR);
        return 0;
    }
    snprintf(p, sizeof p, "%s\\SAVEGAME", hd);
    make_dirs(p);
    snprintf(p, sizeof p, "%s\\WORLD", hd);
    make_dirs(p);
    return 1;
}

/* the exe's icon comes from the game's own sgens.exe */
const char *game_icon_exe(void) { return "sgens.exe"; }
const char *game_icon_lbm(void) { return GAME_ICON_LBM; }
const char *game_icon_ico(void) { return GAME_ICON_ICO; }
