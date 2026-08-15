#ifndef GUIDE_TEXT_H__
#define GUIDE_TEXT_H__

#include <SDL/SDL_ttf.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/file.h"
#include "utils/hash.h"
#include "utils/log.h"
#include "utils/str.h"

#define GUIDE_CURSOR_DIR "/mnt/SDCARD/Saves/CurrentProfile/guides"
#define GUIDE_LINE_MAX 1024
// Guides are normally hard-wrapped, so a hard line is short. Bound the
// backwards scan anyway so a single-line file can't make scrolling up O(file).
#define GUIDE_BACKSCAN_MAX 8192

typedef struct GuideText {
    char *text;
    size_t len;
    TTF_Font *font;
    int wrap_width;
} GuideText_s;

typedef struct GuideLine {
    size_t start;
    size_t len;
} GuideLine_s;

/**
 * @brief Read a guide into memory. Tabs are expanded to spaces and CRLF is
 *        normalised, so ASCII maps from GameFAQs-style guides line up.
 */
bool guide_textLoad(GuideText_s *g, const char *path)
{
    memset(g, 0, sizeof(GuideText_s));

    char *raw = file_read(path);
    if (raw == NULL)
        return false;

    size_t raw_len = strlen(raw);
    // Worst case every tab becomes 4 spaces
    char *text = (char *)malloc(raw_len * 4 + 1);
    if (text == NULL) {
        free(raw);
        return false;
    }

    size_t out = 0;
    int column = 0;
    for (size_t i = 0; i < raw_len; i++) {
        char c = raw[i];
        if (c == '\r')
            continue;
        if (c == '\t') {
            int spaces = 4 - (column % 4);
            for (int s = 0; s < spaces; s++)
                text[out++] = ' ';
            column += spaces;
            continue;
        }
        text[out++] = c;
        column = (c == '\n') ? 0 : column + 1;
    }
    text[out] = '\0';
    free(raw);

    g->text = text;
    g->len = out;
    return true;
}

void guide_textFree(GuideText_s *g)
{
    if (g->text != NULL) {
        free(g->text);
        g->text = NULL;
    }
    g->len = 0;
}

static int _guide_measure(GuideText_s *g, size_t start, size_t len)
{
    char buf[GUIDE_LINE_MAX];
    if (len >= sizeof(buf))
        len = sizeof(buf) - 1;
    memcpy(buf, g->text + start, len);
    buf[len] = '\0';

    int w = 0, h = 0;
    if (TTF_SizeUTF8(g->font, buf, &w, &h) != 0)
        return 0;
    return w;
}

static size_t _guide_hardLineEnd(GuideText_s *g, size_t offset)
{
    while (offset < g->len && g->text[offset] != '\n')
        offset++;
    return offset;
}

static size_t _guide_hardLineStart(GuideText_s *g, size_t offset)
{
    size_t limit = (offset > GUIDE_BACKSCAN_MAX) ? offset - GUIDE_BACKSCAN_MAX : 0;
    while (offset > limit && g->text[offset - 1] != '\n')
        offset--;
    return offset;
}

/**
 * @brief Break a word that is too long to fit on its own line.
 */
static size_t _guide_breakWord(GuideText_s *g, size_t start, size_t hard_end)
{
    size_t lo = start + 1, hi = hard_end, best = start + 1;

    while (lo <= hi) {
        size_t mid = lo + (hi - lo) / 2;
        // don't split inside a utf-8 sequence
        while (mid > start + 1 && ((unsigned char)g->text[mid] & 0xC0) == 0x80)
            mid--;
        if (_guide_measure(g, start, mid - start) <= g->wrap_width) {
            best = mid;
            lo = mid + 1;
        }
        else {
            if (mid == start + 1)
                break;
            hi = mid - 1;
        }
    }

    return best;
}

/**
 * @brief End offset (exclusive) of the single displayed line starting at
 *        `start`, wrapping on word boundaries.
 */
static size_t _guide_lineEnd(GuideText_s *g, size_t start)
{
    size_t hard_end = _guide_hardLineEnd(g, start);

    if (start >= hard_end)
        return hard_end;

    // Fast path: the whole hard line fits (the common case, guides are
    // usually pre-wrapped to ~79 columns)
    if (_guide_measure(g, start, hard_end - start) <= g->wrap_width)
        return hard_end;

    size_t i = start, last_fit = start;

    while (i < hard_end) {
        size_t j = i;
        while (j < hard_end && g->text[j] == ' ')
            j++;
        while (j < hard_end && g->text[j] != ' ')
            j++;

        if (_guide_measure(g, start, j - start) > g->wrap_width) {
            if (last_fit > start)
                return last_fit;
            return _guide_breakWord(g, start, hard_end);
        }

        last_fit = j;
        i = j;
    }

    return hard_end;
}

/**
 * @brief Offset of the displayed line following the one at `start`.
 */
size_t guide_nextLine(GuideText_s *g, size_t start)
{
    size_t hard_end = _guide_hardLineEnd(g, start);
    size_t end = _guide_lineEnd(g, start);

    if (end >= hard_end)
        return (hard_end < g->len) ? hard_end + 1 : g->len;

    while (end < hard_end && g->text[end] == ' ')
        end++;

    return (end > start) ? end : start + 1;
}

/**
 * @brief Offset of the displayed line preceding the one at `offset`.
 */
size_t guide_prevLine(GuideText_s *g, size_t offset)
{
    if (offset == 0)
        return 0;

    size_t hard_start = _guide_hardLineStart(g, offset);

    if (hard_start == offset) {
        if (hard_start == 0)
            return 0;
        offset = hard_start - 1; // the '\n' ending the previous hard line
        hard_start = _guide_hardLineStart(g, offset);
    }

    size_t cur = hard_start, prev = hard_start;
    while (cur < offset) {
        size_t next = guide_nextLine(g, cur);
        if (next <= cur)
            break;
        prev = cur;
        cur = next;
    }

    return prev;
}

/**
 * @brief Fill `lines` with up to `max_lines` displayed lines from `start`.
 * @return number of lines written.
 */
int guide_layout(GuideText_s *g, size_t start, GuideLine_s *lines, int max_lines)
{
    int count = 0;
    size_t cursor = start;

    while (count < max_lines && cursor < g->len) {
        size_t end = _guide_lineEnd(g, cursor);
        lines[count].start = cursor;
        lines[count].len = end - cursor;
        count++;

        size_t next = guide_nextLine(g, cursor);
        if (next <= cursor)
            break;
        cursor = next;
    }

    return count;
}

size_t guide_scroll(GuideText_s *g, size_t cursor, int lines)
{
    while (lines > 0 && cursor < g->len) {
        size_t next = guide_nextLine(g, cursor);
        if (next <= cursor || next >= g->len)
            break;
        cursor = next;
        lines--;
    }
    while (lines < 0 && cursor > 0) {
        cursor = guide_prevLine(g, cursor);
        lines++;
    }
    return cursor;
}

static void _guide_cursorPath(const char *guide_path, char *out, size_t out_size)
{
    snprintf(out, out_size, GUIDE_CURSOR_DIR "/%" PRIu32 ".pos",
             FNV1A_Pippip_Yurii(guide_path, strlen(guide_path)));
}

size_t guide_cursorLoad(const char *guide_path, size_t max)
{
    char path[STR_MAX];
    unsigned long cursor = 0;

    _guide_cursorPath(guide_path, path, sizeof(path));

    FILE *fp = fopen(path, "r");
    if (fp == NULL)
        return 0;
    if (fscanf(fp, "%lu", &cursor) != 1)
        cursor = 0;
    fclose(fp);

    return (cursor > max) ? 0 : (size_t)cursor;
}

void guide_cursorSave(const char *guide_path, size_t cursor)
{
    char path[STR_MAX];

    mkdirs(GUIDE_CURSOR_DIR);
    _guide_cursorPath(guide_path, path, sizeof(path));

    FILE *fp = fopen(path, "w");
    if (fp == NULL) {
        printf_debug("failed to save guide cursor: %s\n", path);
        return;
    }
    fprintf(fp, "%lu\n", (unsigned long)cursor);
    fclose(fp);
}

#endif // GUIDE_TEXT_H__
