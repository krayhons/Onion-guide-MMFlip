#ifndef RA_STATE_H__
#define RA_STATE_H__

/**
 * Reliable pause/unpause for RetroArch.
 *
 * Onion's udp_send() pings RetroArch with "VERSION" and waits 100ms for a
 * reply before it will send anything; if that ping times out the real command
 * is silently dropped and the caller gets no useful error. A paused RetroArch
 * throttles its main loop, so it answers that ping late often enough that an
 * UNPAUSE simply never arrives - which leaves the game frozen with whatever
 * was last drawn still on screen.
 *
 * So: send the command directly, then ask RetroArch what state it is actually
 * in, and keep going until it agrees with us. Verifying beats hoping, and it
 * also covers the case where something else has already paused the emulator.
 */

#include <arpa/inet.h>
#include <stdbool.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "utils/log.h"
#include "utils/msleep.h"

#define RA_HOST "127.0.0.1"
#define RA_PORT 55355
#define RA_REPLY_TIMEOUT_MS 150
#define RA_SETTLE_MS 60
#define RA_MAX_ATTEMPTS 8

/**
 * @brief Send one command to RetroArch, optionally waiting for its reply.
 *        No VERSION ping, no retry loop - just the datagram.
 * @return 0 on success (and on reply when one was asked for), -1 otherwise.
 */
static int ra_command(const char *cmd, char *response, size_t response_size)
{
    struct sockaddr_in addr;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(RA_PORT);
    if (inet_pton(AF_INET, RA_HOST, &addr.sin_addr) <= 0) {
        close(fd);
        return -1;
    }

    if (sendto(fd, cmd, strlen(cmd), 0, (struct sockaddr *)&addr,
               sizeof(addr)) == -1) {
        close(fd);
        return -1;
    }

    if (response == NULL || response_size == 0) {
        close(fd);
        return 0;
    }

    struct timeval timeout;
    timeout.tv_sec = RA_REPLY_TIMEOUT_MS / 1000;
    timeout.tv_usec = (RA_REPLY_TIMEOUT_MS % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    ssize_t received = recvfrom(fd, response, response_size - 1, 0, NULL, NULL);
    close(fd);

    if (received <= 0)
        return -1;

    response[received] = '\0';
    return 0;
}

typedef enum RaState {
    RA_STATE_UNKNOWN,
    RA_STATE_PLAYING,
    RA_STATE_PAUSED,
    RA_STATE_CONTENTLESS,
    RA_STATE_NO_REPLY
} RaState_e;

static RaState_e ra_getState(void)
{
    char response[256];
    char word[32];

    if (ra_command("GET_STATUS", response, sizeof(response)) != 0)
        return RA_STATE_NO_REPLY;

    if (sscanf(response, "GET_STATUS %31s", word) != 1)
        return RA_STATE_UNKNOWN;

    if (strcmp(word, "PLAYING") == 0)
        return RA_STATE_PLAYING;
    if (strcmp(word, "PAUSED") == 0)
        return RA_STATE_PAUSED;
    if (strcmp(word, "CONTENTLESS") == 0)
        return RA_STATE_CONTENTLESS;

    return RA_STATE_UNKNOWN;
}

/**
 * @brief Drive RetroArch to the requested pause state and confirm it got there.
 * @return true when RetroArch reports the state we asked for.
 */
static bool ra_setPaused(bool paused)
{
    int silent = 0;

    for (int attempt = 0; attempt < RA_MAX_ATTEMPTS; attempt++) {
        RaState_e state = ra_getState();

        if (state == RA_STATE_PAUSED && paused)
            return true;
        if (state == RA_STATE_PLAYING && !paused)
            return true;

        if (state == RA_STATE_CONTENTLESS)
            return false; // no game loaded, nothing to pause

        if (state == RA_STATE_NO_REPLY) {
            // RetroArch may simply be busy; give up once it has ignored us
            // several times in a row, it has most likely gone away
            if (++silent >= 3)
                return false;
        }
        else {
            silent = 0;
        }

        if (paused)
            ra_command("PAUSE", NULL, 0);
        else
            ra_command("UNPAUSE", NULL, 0);

        msleep(RA_SETTLE_MS);
    }

    printf_debug("failed to %s RetroArch\n", paused ? "pause" : "resume");
    return false;
}

#endif // RA_STATE_H__
