#pragma once
#include "setup.h"

#include "game.h"
#define UNINSTALL_KEY "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\" GAME_ID "_PCNative"

typedef struct {
    char cd[MAX_PATH * 2], cd2[MAX_PATH * 2], dir[MAX_PATH * 2];
    int desktop, startmenu;
} InstallOpts;

int do_install(const InstallOpts *o, void (*cb)(u64 done, u64 total, void *user), void *user);
int do_uninstall(const char *dir);
int prebuilt_available(void);
/* installer/game_install.c */
int game_is_cd(CdSrc *c);                                 /* disc 1 */
int game_is_cd2(CdSrc *c);                                /* disc 2 */
const char *game_icon_exe(void);                          /* exe in hd whose icon the port gets, or NULL */
int inject_icon_from_exe(const char *exe, const char *src);
/* installer/is3.c: InstallShield 3 .Z archives */
int is3_extract(const char *archive, const char *outdir, const char *const *want);
int game_install_files(const char *cd, const char *hd);   /* what the DOS installer put on C: */
const char *game_icon_lbm(void);                          /* LBM on the CD for the exe icon, or NULL */
int inject_icon_lbm(const char *exe, const u8 *lbm, size_t n);
int inject_icon_ico(const char *exe, const u8 *ico, size_t n);
const char *game_icon_ico(void);
void shortcut_paths(char *startmenu, char *desktop);
