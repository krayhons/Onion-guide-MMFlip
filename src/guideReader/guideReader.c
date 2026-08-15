#include <SDL/SDL.h>
#include <SDL/SDL_image.h>
#include <SDL/SDL_ttf.h>
#include <libgen.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "components/list.h"
#include "system/battery.h"
#include "system/display.h"
#include "system/keymap_sw.h"
#include "system/lang.h"
#include "system/settings.h"
#include "system/state.h"
#include "theme/theme.h"
#include "utils/config.h"
#include "utils/file.h"
#include "utils/flags.h"
#include "utils/log.h"
#include "utils/msleep.h"
#include "utils/retroarch_cmd.h"
#include "utils/sdl_direct_fb.h"
#include "utils/str.h"

#include "guide_finder.h"
#include "guide_text.h"
#include "ra_state.h"

#define FRAMES_PER_SECOND 30
#define REPEAT_DELAY_MS 320
#define REPEAT_RATE_MS 60

#define FONT_SIZE_MIN 12
#define FONT_SIZE_MAX 32
#define FONT_SIZE_DEFAULT 18
#define FONT_SIZE_KEY "guideReader/fontSize"

#define GUIDE_OPEN_FLAG "guideReader_open"

static bool quit = false;
static bool is_overlay = false;
static bool restored = false;

static int _repeat_counter[320] = {0};
static uint32_t _repeat_time[320] = {0};
static KeyState _keystate[320] = {(KeyState)0};

/**
 * @brief Put the system back the way we found it: hand back the input device,
 *        let the game run again, and drop the "guide is open" flag.
 *
 * Everything that unfreezes the console lives in here, it is idempotent, and
 * it runs from atexit() and from the crash handlers as well as on the normal
 * path. If any part of the teardown below it goes wrong, the console still
 * comes back - the alternative is a game left paused forever with a stale
 * flag file that stops the guide from ever opening again.
 */
static void _restoreSystem(void)
{
    if (restored)
        return;
    restored = true;

    keyinput_enable(); // no-op if we already did it on the normal path

    if (is_overlay)
        ra_setPaused(false); // verifies, rather than firing UNPAUSE and hoping

    temp_flag_set(GUIDE_OPEN_FLAG, false);
}

static void sigHandler(int sig)
{
    switch (sig) {
    case SIGINT:
    case SIGTERM:
        quit = true;
        break;
    default:
        break;
    }
}

static void crashHandler(int sig)
{
    _restoreSystem();
    signal(sig, SIG_DFL);
    raise(sig);
}

/**
 * @brief True when a key should act: once on press, then repeating while it
 *        stays held. (The direct-framebuffer input path doesn't deliver
 *        kernel auto-repeat, so we time it ourselves.)
 */
static bool _keyRepeat(KeyState keystate[320], SDLKey key)
{
    if (keystate[key] < PRESSED) {
        _repeat_counter[key] = 0;
        return false;
    }

    uint32_t now = SDL_GetTicks();

    if (_repeat_counter[key] == 0) {
        _repeat_counter[key] = 1;
        _repeat_time[key] = now;
        return true;
    }

    uint32_t delay = (_repeat_counter[key] == 1) ? REPEAT_DELAY_MS : REPEAT_RATE_MS;

    if (now - _repeat_time[key] >= delay) {
        _repeat_counter[key]++;
        _repeat_time[key] = now;
        return true;
    }

    return false;
}

static bool _keyPressed(KeyState keystate[320], SDLKey key)
{
    if (keystate[key] < PRESSED) {
        _repeat_counter[key] = 0;
        return false;
    }
    return _repeat_counter[key]++ == 0;
}

/**
 * @brief Monospace reads best for guides (ASCII maps, tables), so prefer one
 *        if the user dropped one in, or if the Ebook Reader package is
 *        installed. Otherwise fall back to the theme's list font.
 */
static TTF_Font *_loadGuideFont(int size)
{
    static const char *mono_fonts[] = {
        "/mnt/SDCARD/.tmp_update/res/GuideMono.ttf",
        "/mnt/SDCARD/App/PixelReader/resources/fonts/DejaVuSansMono.ttf",
        NULL};

    for (int i = 0; mono_fonts[i] != NULL; i++) {
        if (!exists(mono_fonts[i]))
            continue;
        TTF_Font *font = TTF_OpenFont(mono_fonts[i], (int)(size * g_scale));
        if (font != NULL) {
            printf_debug("using guide font: %s\n", mono_fonts[i]);
            return font;
        }
    }

    return theme_loadFont(theme()->path, theme()->list.font, size);
}

/**
 * @brief Grab the frame the (paused) game left in the framebuffer and darken
 *        it, so the reader floats on top of the game instead of a blank
 *        screen.
 */
static SDL_Surface *_captureBackdrop(void)
{
    SDL_Surface *backdrop = SDL_CreateRGBSurface(SDL_SWSURFACE, g_display.width,
                                                 g_display.height, 32, 0, 0, 0, 0);
    if (backdrop == NULL)
        return NULL;

    display_readCurrentBuffer(&g_display, (uint32_t *)backdrop->pixels,
                              (rect_t){0, 0, g_display.width, g_display.height},
                              true, false);

    SDL_Surface *shade = SDL_CreateRGBSurface(0, g_display.width, g_display.height,
                                              32, 0x00FF0000, 0x0000FF00,
                                              0x000000FF, 0xFF000000);
    if (shade != NULL) {
        SDL_FillRect(shade, NULL, 0xE6000000);
        SDL_BlitSurface(shade, NULL, backdrop, NULL);
        SDL_FreeSurface(shade);
    }

    return backdrop;
}

static void _renderPercentage(SDL_Surface *screen, int percent)
{
    char label[8];
    snprintf(label, sizeof(label), "%d%%", percent);

    SDL_Surface *text = TTF_RenderUTF8_Blended(resource_getFont(HINT), label,
                                              theme()->hint.color);
    if (text == NULL)
        return;

    SDL_Rect pos = {g_display.width - text->w - (int)(20.0 * g_scale),
                    g_display.height - (int)(30.0 * g_scale) - text->h / 2};
    SDL_BlitSurface(text, NULL, screen, &pos);
    SDL_FreeSurface(text);
}

static void _renderReader(SDL_Surface *screen, SDL_Surface *backdrop,
                          GuideText_s *guide, const char *title, size_t cursor,
                          int battery_percentage)
{
    int header_height = (int)(60.0 * g_scale);
    int footer_height = (int)(60.0 * g_scale);
    int margin_x = (int)(20.0 * g_scale);
    int content_top = header_height + (int)(8.0 * g_scale);
    int content_height = g_display.height - footer_height - content_top;

    int line_height = TTF_FontLineSkip(guide->font);
    int max_lines = (line_height > 0) ? content_height / line_height : 1;
    if (max_lines < 1)
        max_lines = 1;

    if (backdrop != NULL)
        SDL_BlitSurface(backdrop, NULL, screen, NULL);
    else
        SDL_FillRect(screen, NULL, 0);

    GuideLine_s lines[64];
    if (max_lines > 64)
        max_lines = 64;

    int count = guide_layout(guide, cursor, lines, max_lines);

    char buf[GUIDE_LINE_MAX];
    for (int i = 0; i < count; i++) {
        size_t len = lines[i].len;
        if (len == 0)
            continue;
        if (len >= sizeof(buf))
            len = sizeof(buf) - 1;
        memcpy(buf, guide->text + lines[i].start, len);
        buf[len] = '\0';

        SDL_Surface *text = TTF_RenderUTF8_Blended(guide->font, buf,
                                                   theme()->list.color);
        if (text == NULL)
            continue;

        SDL_Rect pos = {margin_x, content_top + i * line_height};
        SDL_BlitSurface(text, NULL, screen, &pos);
        SDL_FreeSurface(text);
    }

    theme_renderHeader(screen, title, false);
    theme_renderHeaderBattery(screen, battery_percentage);
    theme_renderFooter(screen);
    theme_renderStandardHint(screen, lang_get(LANG_NEXT, LANG_FALLBACK_NEXT),
                             lang_get(LANG_BACK, LANG_FALLBACK_BACK));
    _renderPercentage(screen, guide->len > 0
                                  ? (int)((cursor * 100) / guide->len)
                                  : 100);
}

/**
 * @brief Read one guide until the user backs out.
 * @return true when the whole app should close, false to go back to the
 *         guide list.
 */
static bool _readGuide(SDL_Surface *backdrop, const char *guide_path,
                       int *font_size)
{
    GuideText_s guide;

    if (!guide_textLoad(&guide, guide_path)) {
        theme_renderDialog(screen, "Guide", "Could not read guide file.", true);
        render();
        msleep(1500);
        return false;
    }

    guide.font = _loadGuideFont(*font_size);
    guide.wrap_width = g_display.width - (int)(40.0 * g_scale);

    if (guide.font == NULL) {
        guide_textFree(&guide);
        return true;
    }

    char title_buf[GUIDE_PATH_MAX];
    strncpy(title_buf, guide_path, GUIDE_PATH_MAX - 1);
    title_buf[GUIDE_PATH_MAX - 1] = '\0';
    char *title = file_removeExtension(basename(title_buf));
    size_t cursor = guide_cursorLoad(guide_path, guide.len);

    int battery_percentage = battery_getPercentage();
    int line_height = TTF_FontLineSkip(guide.font);
    int page_lines = (g_display.height - (int)(128.0 * g_scale)) /
                     (line_height > 0 ? line_height : 1);
    if (page_lines < 1)
        page_lines = 1;

    KeyState *keystate = _keystate;
    bool changed = true;
    bool close_app = false;
    bool back = false;

    uint32_t acc_ticks = 0, last_ticks = SDL_GetTicks(),
             time_step = 1000 / FRAMES_PER_SECOND;

    while (!quit && !back && !close_app) {
        uint32_t ticks = SDL_GetTicks();
        acc_ticks += ticks - last_ticks;
        last_ticks = ticks;

        _updateKeystate(keystate, &quit, true, NULL);

        // Every key is sampled every pass - deliberately using | and not ||.
        // These helpers keep per-key state, so short-circuiting (or an
        // else-if chain) would let a held direction key swallow a B press and
        // make going back feel unreliable.
        bool k_line_up = _keyRepeat(keystate, SW_BTN_UP);
        bool k_line_down = _keyRepeat(keystate, SW_BTN_DOWN);
        bool k_page_up = _keyRepeat(keystate, SW_BTN_LEFT) |
                         _keyRepeat(keystate, SW_BTN_L1);
        bool k_page_down = _keyRepeat(keystate, SW_BTN_RIGHT) |
                           _keyRepeat(keystate, SW_BTN_R1) |
                           _keyRepeat(keystate, SW_BTN_A);
        bool k_smaller = _keyPressed(keystate, SW_BTN_L2);
        bool k_bigger = _keyPressed(keystate, SW_BTN_R2);
        bool k_back = _keyPressed(keystate, SW_BTN_B);
        bool k_close = _keyPressed(keystate, SW_BTN_MENU) |
                       _keyPressed(keystate, SW_BTN_START);

        size_t next = cursor;

        // Leaving always wins over scrolling
        if (k_close)
            close_app = true;
        else if (k_back)
            back = true;
        else if (k_line_up)
            next = guide_scroll(&guide, cursor, -1);
        else if (k_line_down)
            next = guide_scroll(&guide, cursor, 1);
        else if (k_page_up)
            next = guide_scroll(&guide, cursor, -page_lines);
        else if (k_page_down)
            next = guide_scroll(&guide, cursor, page_lines);
        else if (k_smaller || k_bigger) {
            int size = *font_size + (k_bigger ? 2 : -2);
            if (size >= FONT_SIZE_MIN && size <= FONT_SIZE_MAX) {
                TTF_Font *resized = _loadGuideFont(size);
                if (resized != NULL) {
                    TTF_CloseFont(guide.font);
                    guide.font = resized;
                    *font_size = size;
                    config_setNumber(FONT_SIZE_KEY, size);
                    line_height = TTF_FontLineSkip(guide.font);
                    page_lines = (g_display.height - (int)(128.0 * g_scale)) /
                                 (line_height > 0 ? line_height : 1);
                    if (page_lines < 1)
                        page_lines = 1;
                    changed = true;
                }
            }
        }

        if (next != cursor) {
            cursor = next;
            changed = true;
        }

        if (acc_ticks >= time_step) {
            acc_ticks -= time_step;

            if (battery_hasChanged(ticks, &battery_percentage))
                changed = true;

            if (changed) {
                _renderReader(screen, backdrop, &guide, title, cursor,
                              battery_percentage);
                render();
                changed = false;
            }
        }

        msleep(4);
    }

    guide_cursorSave(guide_path, cursor);

    free(title);
    TTF_CloseFont(guide.font);
    guide_textFree(&guide);

    return close_app;
}

/**
 * @brief Pick from several guides for the same game.
 * @return index of the chosen guide, or -1 to quit.
 */
static int _selectGuide(SDL_Surface *backdrop, GuideList_s *guides,
                        int selected)
{
    List list = list_createWithTitle(guides->count, LIST_SMALL, "Guides");

    char name_buf[GUIDE_PATH_MAX];
    for (int i = 0; i < guides->count; i++) {
        strncpy(name_buf, guides->paths[i], GUIDE_PATH_MAX - 1);
        name_buf[GUIDE_PATH_MAX - 1] = '\0';
        char *name = file_removeExtension(basename(name_buf));
        ListItem item = {.item_type = ACTION};
        strncpy(item.label, name, STR_MAX - 1);
        list_addItem(&list, item);
        free(name);
    }

    list_scrollTo(&list, selected);

    int battery_percentage = battery_getPercentage();
    KeyState *keystate = _keystate;
    bool changed = true;
    int chosen = -1;

    uint32_t acc_ticks = 0, last_ticks = SDL_GetTicks(),
             time_step = 1000 / FRAMES_PER_SECOND;

    while (!quit && chosen == -1) {
        uint32_t ticks = SDL_GetTicks();
        acc_ticks += ticks - last_ticks;
        last_ticks = ticks;

        _updateKeystate(keystate, &quit, true, NULL);

        bool k_up = _keyRepeat(keystate, SW_BTN_UP);
        bool k_down = _keyRepeat(keystate, SW_BTN_DOWN);
        bool k_select = _keyPressed(keystate, SW_BTN_A);
        bool k_back = _keyPressed(keystate, SW_BTN_B) |
                      _keyPressed(keystate, SW_BTN_MENU) |
                      _keyPressed(keystate, SW_BTN_START);

        if (k_back)
            break;
        else if (k_select)
            chosen = list.active_pos;
        else if (k_up)
            changed |= list_keyUp(&list, false);
        else if (k_down)
            changed |= list_keyDown(&list, false);

        if (acc_ticks >= time_step) {
            acc_ticks -= time_step;

            if (battery_hasChanged(ticks, &battery_percentage))
                changed = true;

            if (changed) {
                if (backdrop != NULL)
                    SDL_BlitSurface(backdrop, NULL, screen, NULL);
                else
                    SDL_FillRect(screen, NULL, 0);
                theme_renderList(screen, &list);
                theme_renderHeader(screen, "Guides", false);
                theme_renderHeaderBattery(screen, battery_percentage);
                theme_renderFooter(screen);
                theme_renderStandardHint(
                    screen, lang_get(LANG_SELECT, LANG_FALLBACK_SELECT),
                    lang_get(LANG_BACK, LANG_FALLBACK_BACK));
                render();
                changed = false;
            }
        }

        msleep(4);
    }

    list_free(&list);

    return chosen;
}

/**
 * @brief Wait for the buttons to come back up before releasing the input
 *        grab, so keymon doesn't see a lone MENU release and fire the
 *        single-press action (i.e. drop straight into the GameSwitcher).
 */
static void _waitForRelease(void)
{
    uint32_t start = SDL_GetTicks();

    while (SDL_GetTicks() - start < 1500) {
        _updateKeystate(_keystate, &quit, true, NULL);

        if (_keystate[SW_BTN_MENU] < PRESSED && _keystate[SW_BTN_START] < PRESSED &&
            _keystate[SW_BTN_A] < PRESSED && _keystate[SW_BTN_B] < PRESSED)
            return;

        msleep(8);
    }
}

static void _showMessage(const char *title, const char *message)
{
    KeyState *keystate = _keystate;
    uint32_t start = SDL_GetTicks();

    theme_renderDialog(screen, title, message, true);
    render();

    // Dismiss on any button, or after a few seconds
    while (!quit && SDL_GetTicks() - start < 4000) {
        _updateKeystate(keystate, &quit, true, NULL);

        if (SDL_GetTicks() - start > 300 &&
            (keystate[SW_BTN_A] >= PRESSED || keystate[SW_BTN_B] >= PRESSED ||
             keystate[SW_BTN_MENU] >= PRESSED || keystate[SW_BTN_START] >= PRESSED))
            break;

        msleep(16);
    }
}

int main(int argc, char *argv[])
{
    char rom_path[STR_MAX] = "";
    char guide_override[GUIDE_PATH_MAX] = "";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--overlay") == 0)
            is_overlay = true;
        else if ((strcmp(argv[i], "--rom") == 0) && i + 1 < argc)
            strncpy(rom_path, argv[++i], STR_MAX - 1);
        else if ((strcmp(argv[i], "--file") == 0) && i + 1 < argc)
            strncpy(guide_override, argv[++i], GUIDE_PATH_MAX - 1);
    }

    log_setName("guideReader");
    print_debug("\n\nDebug logging enabled");

    // A second instance would fight over the framebuffer and the input grab
    if (temp_flag_get(GUIDE_OPEN_FLAG)) {
        print_debug("guideReader already running");
        return EXIT_SUCCESS;
    }
    temp_flag_set(GUIDE_OPEN_FLAG, true);

    signal(SIGINT, sigHandler);
    signal(SIGTERM, sigHandler);

    // If anything below goes wrong, still give the console back
    atexit(_restoreSystem);
    signal(SIGSEGV, crashHandler);
    signal(SIGBUS, crashHandler);
    signal(SIGFPE, crashHandler);
    signal(SIGABRT, crashHandler);

    init(INIT_PNG | INIT_TTF | INIT_INPUT);

    settings_load();
    lang_load();

    // Take the input device for ourselves: this keeps our button presses out
    // of the paused game, and stops keymon from acting on them too.
    keyinput_disable();

    // Whoever launched us sent a PAUSE, but that command goes through a ping
    // that can time out and drop it. Confirm the game is actually stopped
    // before we take the screen off it.
    if (is_overlay)
        ra_setPaused(true);

    SDL_Surface *backdrop = _captureBackdrop();

    GuideList_s guides;

    if (strlen(guide_override) > 0) {
        memset(&guides, 0, sizeof(guides));
        strncpy(guides.paths[0], guide_override, GUIDE_PATH_MAX - 1);
        guides.count = is_file(guide_override) ? 1 : 0;
    }
    else {
        if (strlen(rom_path) == 0 && history_getRecentPath(rom_path) == NULL)
            rom_path[0] = '\0';

        printf_debug("rom path: %s\n", rom_path);

        if (strlen(rom_path) == 0 || !guide_find(rom_path, &guides))
            guides.count = 0;
    }

    if (guides.count == 0) {
        _showMessage("No guide found",
                     "Put a .txt guide next to your rom in a\n"
                     "\"Guides\" folder, named after the rom:\n"
                     "Roms/<System>/Guides/<Game>.txt");
    }
    else {
        int font_size = FONT_SIZE_DEFAULT;
        config_get(FONT_SIZE_KEY, CONFIG_INT, &font_size);
        if (font_size < FONT_SIZE_MIN || font_size > FONT_SIZE_MAX)
            font_size = FONT_SIZE_DEFAULT;

        int selected = 0;
        bool done = false;

        while (!quit && !done) {
            if (guides.count > 1) {
                selected = _selectGuide(backdrop, &guides, selected);
                if (selected < 0)
                    break;
            }

            done = _readGuide(backdrop, guides.paths[selected], &font_size) ||
                   guides.count == 1;
        }
    }

    if (backdrop != NULL)
        SDL_FreeSurface(backdrop);

    _waitForRelease();

    // Hand the input device back straight away - that part touches nothing
    // else and there is no reason to hold it through the teardown
    keyinput_enable();

    lang_free();
    resources_free();
    deinit();

    // Unpause only after deinit(), because display_close() pans the
    // framebuffer back to buffer 0 and doing that to a running emulator
    // would show it a stale frame. If anything above dies first, the
    // atexit() and crash handlers still run this.
    _restoreSystem();

    return EXIT_SUCCESS;
}
