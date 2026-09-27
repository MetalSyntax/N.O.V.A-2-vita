/*
 * Copyright (C) 2021      Andy Nguyen
 * Copyright (C) 2022      Rinnegatamante
 * Copyright (C) 2022-2024 Volodymyr Atamanenko
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#include "reimpl/io.h"

#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdarg.h>
#include <psp2/kernel/threadmgr.h>

#ifdef USE_SCELIBC_IO
#include <libc_bridge/libc_bridge.h>
#endif

#include "utils/logger.h"
#include "utils/utils.h"

// Includes the following inline utilities:
// int oflags_musl_to_newlib(int flags);
// dirent64_bionic * dirent_newlib_to_bionic(struct dirent* dirent_newlib);
// void stat_newlib_to_bionic(struct stat * src, stat64_bionic * dst);
#include "reimpl/bits/_struct_converters.c"

#ifndef DATA_PATH
#define DATA_PATH "ux0:data/nova2/"
#endif

// The game hardcodes its Android home and sdcard paths:
//   /data/data/com.gameloft.android.GAND.GloftN2HP[/...]  -> saves (g_saveDirPath:
//       game_save.dat, level_save.dat, settings.dat, interface.dat)
//   /sdcard/gameloft/games/GloftN2HP[/...]  -> game data (*.gla, sounds, ...)
//   /sdcard/[...]  (bare fallback)          -> DATA_PATH
// Layout on the Vita (DATA_PATH has a trailing slash):
//   ux0:data/nova2/            libnova2.so, assets/, res/, logs/
//   ux0:data/nova2/GloftN2HP/  the Android sdcard data folder, copied as-is
//   ux0:data/nova2/saves/      everything under /data/data/<pkg>
static const char *remap_absolute(const char *orig, char *out, size_t outsz) {
    static const char *pkg = "/data/data/com.gameloft.android.GAND.GloftN2HP";
    static const char *sd_game = "/sdcard/gameloft/games/GloftN2HP";
    static const char *sd = "/sdcard/";
    const char *base = DATA_PATH;
    const char *rest = NULL;

    if (strncmp(orig, pkg, strlen(pkg)) == 0) {
        rest = orig + strlen(pkg);
        base = DATA_PATH "saves/";
    } else if (strncmp(orig, sd_game, strlen(sd_game)) == 0) {
        rest = orig + strlen(sd_game);
        base = DATA_PATH "GloftN2HP/";
    } else if (strncmp(orig, sd, strlen(sd)) == 0) {
        rest = orig + strlen(sd);
    } else {
        return orig;
    }
    while (*rest == '/')
        rest++;
    snprintf(out, outsz, "%s%s", base, rest);
    return out;
}

// The engine's working directory, kept as the *Android* path it passed to
// chdir(). glitch's CFileSystem does chdir("/sdcard/gameloft/games/GloftN2HP")
// in appInit and then opens every pack relative ("sprites.gla",
// "sounds_hi.glza", ...). newlib's chdir can't take the Android path, and
// SceLibcBridge (USE_SCELIBC_IO) ignores newlib's cwd anyway, so relative
// paths are resolved here. Defaults to the data folder.
static char g_cwd[PATH_MAX] = "/sdcard/gameloft/games/GloftN2HP";

static int is_relative(const char *p) {
    return p[0] != '/' && strchr(p, ':') == NULL;
}

static const char *soloader_remap_path(const char *orig, char *out, size_t outsz) {
    if (!orig)
        return orig;
    if (is_relative(orig)) {
        char abs[PATH_MAX];
        while (orig[0] == '.' && orig[1] == '/')
            orig += 2;
        snprintf(abs, sizeof(abs), "%s/%s", g_cwd, orig);
        const char *r = remap_absolute(abs, out, outsz);
        if (r == abs) { // cwd outside the remapped trees: keep it absolute
            snprintf(out, outsz, "%s", abs);
            return out;
        }
        return r;
    }
    return remap_absolute(orig, out, outsz);
}

int chdir_soloader(const char *path) {
    char remapped[PATH_MAX];
    char abs[PATH_MAX];
    if (!path)
        return -1;
    if (is_relative(path))
        snprintf(abs, sizeof(abs), "%s/%s", g_cwd, path);
    else
        snprintf(abs, sizeof(abs), "%s", path);

    const char *real = soloader_remap_path(abs, remapped, sizeof(remapped));
    struct stat st;
    if (stat(real, &st) != 0 || !S_ISDIR(st.st_mode)) {
        l_warn("chdir(%s -> %s): not a directory", path, real);
        return -1;
    }
    size_t n = strlen(abs);
    while (n > 1 && abs[n - 1] == '/')
        abs[--n] = '\0';
    snprintf(g_cwd, sizeof(g_cwd), "%s", abs);
    l_debug("chdir(%s -> %s): 0", path, real);
    return 0;
}

char *getcwd_soloader(char *buf, size_t size) {
    if (!buf) {
        size = size ? size : PATH_MAX;
        buf = malloc(size);
        if (!buf)
            return NULL;
    }
    if (strlen(g_cwd) + 1 > size)
        return NULL;
    strcpy(buf, g_cwd);
    return buf;
}

int lstat_soloader(const char *path, stat64_bionic *buf) {
    // No symlinks on the Vita: lstat == stat (with the bionic struct layout;
    // the raw newlib lstat previously wrote a newlib struct stat into it).
    return stat_soloader(path, buf);
}

/*
 * Caché negativo de fopen de solo lectura (log 0020): en cada sonido el motor
 * prueba primero un .wav suelto que no existe (sfx_fs_run_stone_2.wav...) y
 * recién después lo busca en sounds_hi.glza. Cada fopen fallido va a la
 * memoria (síncrono) + una línea de log síncrona -> tirones en cada paso o
 * disparo. Una ruta que falló en lectura devuelve NULL al instante hasta que
 * algo escriba/cree/renombre/borre archivos (entonces se vacía el caché).
 */
#define NEG_MAX 1024
static char *neg_path[NEG_MAX];
static uint32_t neg_hash[NEG_MAX];
static int neg_count;
static SceKernelLwMutexWork neg_mtx;
static int neg_mtx_ready;

static uint32_t neg_fnv(const char *s) {
    uint32_t h = 2166136261u;
    while (*s)
        h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static void neg_lock(void) {
    if (!neg_mtx_ready) {
        sceKernelCreateLwMutex(&neg_mtx, "neg_fopen", 0, 0, NULL);
        neg_mtx_ready = 1;
    }
    sceKernelLockLwMutex(&neg_mtx, 1, NULL);
}

static int neg_has(const char *path) {
    uint32_t h = neg_fnv(path);
    int hit = 0;
    neg_lock();
    for (int i = 0; i < neg_count; i++)
        if (neg_hash[i] == h && strcmp(neg_path[i], path) == 0) { hit = 1; break; }
    sceKernelUnlockLwMutex(&neg_mtx, 1);
    return hit;
}

static void neg_add(const char *path) {
    neg_lock();
    if (neg_count < NEG_MAX) {
        neg_path[neg_count] = strdup(path);
        if (neg_path[neg_count]) {
            neg_hash[neg_count] = neg_fnv(path);
            neg_count++;
        }
    }
    sceKernelUnlockLwMutex(&neg_mtx, 1);
}

static void neg_clear(void) {
    neg_lock();
    for (int i = 0; i < neg_count; i++)
        free(neg_path[i]);
    neg_count = 0;
    sceKernelUnlockLwMutex(&neg_mtx, 1);
}

static int mode_is_readonly(const char *mode) {
    return mode && mode[0] == 'r' && !strchr(mode, '+');
}

FILE * fopen_soloader(const char * filename, const char * mode) {
    char remapped[PATH_MAX];
    if (strcmp(filename, "/proc/cpuinfo") == 0) {
        return fopen_soloader("app0:/cpuinfo", mode);
    } else if (strcmp(filename, "/proc/meminfo") == 0) {
        return fopen_soloader("app0:/meminfo", mode);
    }
    filename = soloader_remap_path(filename, remapped, sizeof(remapped));

    int ro = mode_is_readonly(mode);
    if (ro && neg_has(filename))
        return NULL;
    if (!ro)
        neg_clear();

#ifdef USE_SCELIBC_IO
    FILE* ret = sceLibcBridge_fopen(filename, mode);
#else
    FILE* ret = fopen(filename, mode);
#endif

    // Sin log en éxito: el motor abre sounds_hi.glza/.gla en cada sonido
    // durante el juego y cada línea es un sceIoWrite síncrono (tirones de FPS,
    // log 0014). Los fallos se siguen logueando.
    if (!ret) {
        l_warn("fopen(%s, %s): %p (se cachea, no se vuelve a loguear)", filename, mode, ret);
        if (ro)
            neg_add(filename);
    }

    return ret;
}

int open_soloader(const char * path, int oflag, ...) {
    if (oflag & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC))
        neg_clear();
    char remapped[PATH_MAX];
    if (strcmp(path, "/proc/cpuinfo") == 0) {
        return open_soloader("app0:/cpuinfo", oflag);
    } else if (strcmp(path, "/proc/meminfo") == 0) {
        return open_soloader("app0:/meminfo", oflag);
    }
    path = soloader_remap_path(path, remapped, sizeof(remapped));

    mode_t mode = 0666;
    if (((oflag & BIONIC_O_CREAT) == BIONIC_O_CREAT) ||
        ((oflag & BIONIC_O_TMPFILE) == BIONIC_O_TMPFILE)) {
        va_list args;
        va_start(args, oflag);
        mode = (mode_t)(va_arg(args, int));
        va_end(args);
    }

    oflag = oflags_bionic_to_newlib(oflag);
    int ret = open(path, oflag, mode);
    if (ret >= 0)
        l_debug("open(%s, %x): %i", path, oflag, ret);
    else
        l_warn("open(%s, %x): %i", path, oflag, ret);
    return ret;
}

int fstat_soloader(int fd, stat64_bionic * buf) {
    struct stat st;
    int res = fstat(fd, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("fstat(%i): %i", fd, res);
    return res;
}

int stat_soloader(const char * path, stat64_bionic * buf) {
    char remapped[PATH_MAX];
    path = soloader_remap_path(path, remapped, sizeof(remapped));
    struct stat st;
    int res = stat(path, &st);

    if (res == 0)
        stat_newlib_to_bionic(&st, buf);

    l_debug("stat(%s): %i", path, res);
    return res;
}

int fclose_soloader(FILE * f) {
#ifdef USE_SCELIBC_IO
    int ret = sceLibcBridge_fclose(f);
#else
    int ret = fclose(f);
#endif

    return ret;
}

int close_soloader(int fd) {
    int ret = close(fd);
    l_debug("close(%i): %i", fd, ret);
    return ret;
}

DIR* opendir_soloader(char* _pathname) {
    char remapped[PATH_MAX];
    _pathname = (char *)soloader_remap_path(_pathname, remapped, sizeof(remapped));
    DIR* ret = opendir(_pathname);
    l_debug("opendir(\"%s\"): %p", _pathname, ret);
    return ret;
}

struct dirent64_bionic * readdir_soloader(DIR * dir) {
    static struct dirent64_bionic dirent_tmp;

    struct dirent* ret = readdir(dir);
    l_debug("readdir(%p): %p", dir, ret);

    if (ret) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(ret);
        memcpy(&dirent_tmp, entry_tmp, sizeof(dirent64_bionic));
        free(entry_tmp);
        return &dirent_tmp;
    }

    return NULL;
}

int readdir_r_soloader(DIR * dirp, dirent64_bionic * entry,
                       dirent64_bionic ** result) {
    struct dirent dirent_tmp;
    struct dirent * pdirent_tmp;

    int ret = readdir_r(dirp, &dirent_tmp, &pdirent_tmp);

    if (ret == 0) {
        dirent64_bionic* entry_tmp = dirent_newlib_to_bionic(&dirent_tmp);
        memcpy(entry, entry_tmp, sizeof(dirent64_bionic));
        *result = (pdirent_tmp != NULL) ? entry : NULL;
        free(entry_tmp);
    }

    l_debug("readdir_r(%p, %p, %p): %i", dirp, entry, result, ret);
    return ret;
}

int closedir_soloader(DIR * dir) {
    int ret = closedir(dir);
    l_debug("closedir(%p): %i", dir, ret);
    return ret;
}

int fcntl_soloader(int fd, int cmd, ...) {
    l_warn("fcntl(%i, %i, ...): not implemented", fd, cmd);
    return 0;
}

int ioctl_soloader(int fd, int request, ...) {
    l_warn("ioctl(%i, %i, ...): not implemented", fd, request);
    return 0;
}

int fsync_soloader(int fd) {
    int ret = fsync(fd);
    l_debug("fsync(%i): %i", fd, ret);
    return ret;
}

int access_soloader(const char * path, int mode) {
    char remapped[PATH_MAX];
    path = soloader_remap_path(path, remapped, sizeof(remapped));
    int ret = access(path, mode);
    l_debug("access(%s, %i): %i", path, mode, ret);
    return ret;
}

int mkdir_soloader(const char * path, mode_t mode) {
    neg_clear();
    char remapped[PATH_MAX];
    path = soloader_remap_path(path, remapped, sizeof(remapped));
    int ret = mkdir(path, mode);
    l_debug("mkdir(%s): %i", path, ret);
    return ret;
}

int remove_soloader(const char * path) {
    neg_clear();
    char remapped[PATH_MAX];
    path = soloader_remap_path(path, remapped, sizeof(remapped));
    int ret = remove(path);
    l_debug("remove(%s): %i", path, ret);
    return ret;
}

int unlink_soloader(const char * path) {
    neg_clear();
    char remapped[PATH_MAX];
    path = soloader_remap_path(path, remapped, sizeof(remapped));
    int ret = unlink(path);
    l_debug("unlink(%s): %i", path, ret);
    return ret;
}

int rename_soloader(const char * old_path, const char * new_path) {
    neg_clear();
    char remapped_old[PATH_MAX], remapped_new[PATH_MAX];
    old_path = soloader_remap_path(old_path, remapped_old, sizeof(remapped_old));
    new_path = soloader_remap_path(new_path, remapped_new, sizeof(remapped_new));
    int ret = rename(old_path, new_path);
    l_debug("rename(%s, %s): %i", old_path, new_path, ret);
    return ret;
}
