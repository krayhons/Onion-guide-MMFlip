/**
 * guidemon - background watcher that opens the in-game guide, so the reader
 * can be added to an existing Onion install as new files only.
 *
 * Two triggers, chosen by the installer and written to
 * config/guidemon/trigger:
 *
 *   combo  (default)  MENU + Y
 *       Nothing in Onion or in RetroArch's shipped config binds that pair, and
 *       keymon suppresses its own MENU actions whenever MENU is held together
 *       with another button. So keymap.json is left completely alone: the
 *       single press still opens the GameSwitcher, the double press still does
 *       whatever it was set to, and no setting exists that Tweaks can't render.
 *
 *   double            double press MENU
 *       Needs the installer to set ingame_double_press to 5, which stock
 *       keymon falls through and ignores while still absorbing the gesture.
 *       That value has no label in stock Tweaks and crashes its settings page,
 *       so the installer only offers this mode alongside a patched Tweaks.
 *
 * Either way keymon keeps ownership of the single and long press - guidemon
 * never reproduces them, so nothing behaves differently from stock except the
 * one gesture that opens the guide.
 */

#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <unistd.h>

#include "system/clock.h"
#include "system/keymap_hw.h"
#include "utils/flags.h"
#include "utils/log.h"
#include "utils/process.h"
#include "utils/retroarch_cmd.h"

#define INPUT_DEVICE "/dev/input/event0"
#define TRIGGER_PATH "/mnt/SDCARD/.tmp_update/config/guidemon/trigger"

// Matching keymon's own timings
#define DOUBLE_PRESS_WINDOW_MS 300
#define LONG_PRESS_MS 700

#define RELEASED 0
#define PRESSED 1
#define REPEAT 2

typedef enum Trigger {
    TRIGGER_COMBO, // MENU + Y
    TRIGGER_DOUBLE // double press MENU
} Trigger_e;

static bool quit = false;

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

static Trigger_e _readTrigger(void)
{
    char word[32] = "";
    FILE *fp = fopen(TRIGGER_PATH, "r");

    if (fp != NULL) {
        if (fscanf(fp, "%31s", word) != 1)
            word[0] = '\0';
        fclose(fp);
    }

    // Anything unrecognised falls back to the mode that changes no settings
    return (strcmp(word, "double") == 0) ? TRIGGER_DOUBLE : TRIGGER_COMBO;
}

static bool _isEmulatorRunning(void)
{
    return process_searchpid("retroarch") || process_searchpid("ra32");
}

static void _openGuide(void)
{
    if (temp_flag_get("guideReader_open")) {
        print_debug("guideReader already open");
        return;
    }

    if (!_isEmulatorRunning()) {
        print_debug("no emulator running, ignoring trigger");
        return;
    }

    print_debug("opening guide");
    retroarch_pause(); // guideReader confirms this actually took effect
    system("guideReader --overlay &");
}

int main(void)
{
    struct input_event ev;
    struct pollfd fds[1];

    log_setName("guidemon");
    print_debug("\n\nDebug logging enabled");

    signal(SIGINT, sigHandler);
    signal(SIGTERM, sigHandler);

    Trigger_e trigger = _readTrigger();
    printf_debug("trigger: %s\n",
                 trigger == TRIGGER_DOUBLE ? "double press MENU" : "MENU + Y");

    int input_fd = open(INPUT_DEVICE, O_RDONLY);
    if (input_fd < 0) {
        perror("guidemon: failed to open input device");
        return EXIT_FAILURE;
    }

    memset(&fds, 0, sizeof(fds));
    fds[0].fd = input_fd;
    fds[0].events = POLLIN;

    bool menu_down = false; // MENU + Y
    int press_time = 0;     // double press
    int keys_down = 0;
    bool combo = false;
    bool armed = false;

    while (!quit) {
        // While guideReader holds the input grab we get nothing here, which is
        // exactly what we want
        if (poll(fds, 1, 1000) <= 0)
            continue;

        if (read(input_fd, &ev, sizeof(ev)) != sizeof(ev))
            continue;

        if (ev.type != EV_KEY || ev.value == REPEAT)
            continue;

        if (trigger == TRIGGER_COMBO) {
            if (ev.code == HW_BTN_MENU)
                menu_down = (ev.value == PRESSED);
            else if (ev.code == HW_BTN_Y && ev.value == PRESSED && menu_down)
                _openGuide();
            continue;
        }

        // TRIGGER_DOUBLE
        if (ev.code != HW_BTN_MENU) {
            if (ev.value == PRESSED) {
                keys_down++;
                combo = true; // MENU + anything is somebody else's shortcut
                armed = false;
            }
            else if (keys_down > 0) {
                keys_down--;
            }
            continue;
        }

        if (ev.value == PRESSED) {
            int now = getMilliseconds();

            if (armed && !combo && keys_down == 0 &&
                now - press_time <= DOUBLE_PRESS_WINDOW_MS) {
                armed = false;
                _openGuide();
            }
            else {
                press_time = now;
                armed = false;
            }
        }
        else { // MENU released
            if (combo || keys_down > 0)
                armed = false;
            else if (getMilliseconds() - press_time >= LONG_PRESS_MS)
                armed = false; // keymon just handled a long press
            else
                armed = true;

            if (keys_down == 0)
                combo = false;
        }
    }

    close(input_fd);

    return EXIT_SUCCESS;
}
