#ifndef GUIDE_FINDER_H__
#define GUIDE_FINDER_H__

#include <dirent.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "utils/file.h"
#include "utils/log.h"
#include "utils/str.h"

#define GUIDES_DIR_NAME "Guides"
#define GUIDES_ROMS_DIR "/mnt/SDCARD/Roms"
#define GUIDE_MAX_FILES 64
#define GUIDE_PATH_MAX (STR_MAX * 2)

typedef struct GuideList {
    char paths[GUIDE_MAX_FILES][GUIDE_PATH_MAX];
    int count;
} GuideList_s;

static const char *GUIDE_EXTENSIONS[] = {"txt", "md", NULL};

static bool _guide_isTextFile(const char *filename)
{
    const char *dot = strrchr(filename, '.');
    if (dot == NULL)
        return false;
    for (int i = 0; GUIDE_EXTENSIONS[i] != NULL; i++)
        if (strcasecmp(dot + 1, GUIDE_EXTENSIONS[i]) == 0)
            return true;
    return false;
}

static int _guide_cmp(const void *a, const void *b)
{
    return strcasecmp((const char *)a, (const char *)b);
}

static void _guide_add(GuideList_s *list, const char *path)
{
    if (list->count >= GUIDE_MAX_FILES)
        return;
    strncpy(list->paths[list->count], path, GUIDE_PATH_MAX - 1);
    list->paths[list->count][GUIDE_PATH_MAX - 1] = '\0';
    list->count++;
}

/**
 * @brief Collect every text file directly inside `dir`, sorted by name.
 */
static void _guide_collectDir(const char *dir, GuideList_s *list)
{
    DIR *dp = opendir(dir);
    if (dp == NULL)
        return;

    struct dirent *ep;
    char full_path[GUIDE_PATH_MAX];

    while ((ep = readdir(dp)) != NULL) {
        if (ep->d_name[0] == '.')
            continue;
        if (!_guide_isTextFile(ep->d_name))
            continue;
        snprintf(full_path, GUIDE_PATH_MAX, "%s/%s", dir, ep->d_name);
        if (is_file(full_path))
            _guide_add(list, full_path);
    }

    closedir(dp);

    if (list->count > 1)
        qsort(list->paths, list->count, GUIDE_PATH_MAX, _guide_cmp);
}

static void _guide_stripExtension(char *path)
{
    char *slash = strrchr(path, '/');
    char *dot = strrchr(path, '.');
    if (dot != NULL && (slash == NULL || dot > slash))
        *dot = '\0';
}

/**
 * @brief Find the guide(s) belonging to a rom.
 *
 * Walks up from the rom towards /mnt/SDCARD/Roms looking for a sibling
 * "Guides" directory. The rom's path relative to that directory's parent is
 * mirrored inside it, e.g. for /mnt/SDCARD/Roms/GBA/Golden Sun.gba:
 *
 *   /mnt/SDCARD/Roms/GBA/Guides/Golden Sun.txt      (single file)
 *   /mnt/SDCARD/Roms/GBA/Guides/Golden Sun/  (a directory of .txt files)
 *   /mnt/SDCARD/Roms/Guides/GBA/Golden Sun.txt      (shared Guides folder)
 *
 * This is the same layout Allium uses, so guide collections are portable
 * between the two firmwares.
 *
 * @param rom_path Absolute path of the running rom.
 * @param list Output, zero-initialised by this function.
 * @return true when at least one guide was found.
 */
bool guide_find(const char *rom_path, GuideList_s *list)
{
    char parent[GUIDE_PATH_MAX];
    char guides_dir[GUIDE_PATH_MAX];
    char candidate[GUIDE_PATH_MAX];

    memset(list, 0, sizeof(GuideList_s));

    if (rom_path == NULL || rom_path[0] != '/')
        return false;

    strncpy(parent, rom_path, GUIDE_PATH_MAX - 1);
    parent[GUIDE_PATH_MAX - 1] = '\0';

    while (true) {
        char *slash = strrchr(parent, '/');
        if (slash == NULL || slash == parent)
            break;
        *slash = '\0';

        snprintf(guides_dir, GUIDE_PATH_MAX, "%s/%s", parent, GUIDES_DIR_NAME);

        if (is_dir(guides_dir)) {
            const char *rel = rom_path + strlen(parent) + 1;
            snprintf(candidate, GUIDE_PATH_MAX, "%s/%s", guides_dir, rel);
            _guide_stripExtension(candidate);

            printf_debug("Checking guide path: %s\n", candidate);

            if (is_dir(candidate)) {
                _guide_collectDir(candidate, list);
                if (list->count > 0)
                    return true;
            }

            size_t base_len = strlen(candidate);
            for (int i = 0; GUIDE_EXTENSIONS[i] != NULL; i++) {
                snprintf(candidate + base_len, GUIDE_PATH_MAX - base_len, ".%s",
                         GUIDE_EXTENSIONS[i]);
                printf_debug("Checking guide file: %s\n", candidate);
                if (is_file(candidate)) {
                    _guide_add(list, candidate);
                    return true;
                }
            }
        }

        if (strcmp(parent, GUIDES_ROMS_DIR) == 0)
            break;
    }

    return false;
}

#endif // GUIDE_FINDER_H__
