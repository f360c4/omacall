#include "calls.h"
#include "config.h"
#include "events.h"
#include "ipc.h"
#include "log.h"
#include "sip.h"
#include "store.h"
#include "util.h"

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/signalfd.h>
#include <unistd.h>

#define MAX_POLL_FDS 16

static oma_config config;
static char config_path[512];
static volatile sig_atomic_t stopping;

static void usage(FILE *out)
{
    fprintf(out,
        "omacalld " OMACALL_VERSION " — SIP softphone daemon for Omarchy\n"
        "\n"
        "Usage: omacalld [options]\n"
        "\n"
        "  -c, --config PATH   configuration file\n"
        "                      (default: $XDG_CONFIG_HOME/omacall/config.json)\n"
        "  -v, --verbose       log at debug level\n"
        "  -V, --version       print the version and exit\n"
        "  -h, --help          print this help and exit\n"
        "\n"
        "The daemon listens on $XDG_RUNTIME_DIR/omacall.sock. Talk to it with\n"
        "the omacall CLI or the Omarchy panel.\n");
}

/* Translate one queued pjsip event into daemon state plus a line on the wire. */
static void handle_event(const oma_event *ev)
{
    switch (ev->type) {
    case OMA_EV_REG_STATE: {
        cJSON *data = cJSON_CreateObject();

        oma_sip_note_reg_state(ev->registered, ev->status_code, ev->status_text);
        cJSON_AddBoolToObject(data, "registered", ev->registered);
        cJSON_AddNumberToObject(data, "status_code", ev->status_code);
        cJSON_AddStringToObject(data, "status_text", ev->status_text);
        cJSON_AddNumberToObject(data, "expires", ev->expires);
        oma_ipc_broadcast("reg.state", data);

        if (ev->registered)
            oma_info("sip: registered (%d %s), expires in %ds",
                     ev->status_code, ev->status_text, ev->expires);
        else
            oma_warn("sip: not registered (%d %s)", ev->status_code, ev->status_text);
        break;
    }
    case OMA_EV_ERROR: {
        cJSON *data = cJSON_CreateObject();

        cJSON_AddStringToObject(data, "code", ev->code);
        cJSON_AddStringToObject(data, "message", ev->message);
        oma_ipc_broadcast("error", data);
        oma_err("sip: %s: %s", ev->code, ev->message);
        break;
    }
    case OMA_EV_CALL_INCOMING:
    case OMA_EV_CALL_STATE:
    case OMA_EV_CALL_ENDED:
    case OMA_EV_MEDIA_STATE:
        oma_calls_handle_event(ev);
        break;
    }
}

static int setup_signalfd(void)
{
    sigset_t mask;

    /* A client that hangs up between our reply and the next broadcast leaves a
     * dead socket behind. Writing to it raises SIGPIPE, whose default action is
     * to kill the process — which is exactly how the daemon died every time the
     * CLI placed a call and exited. Ignore it; the write returns EPIPE and the
     * client is dropped like any other. */
    signal(SIGPIPE, SIG_IGN);

    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);

    /* Block them for every thread, pjsip's included, so the only place they
     * are ever seen is the poll loop below. */
    if (pthread_sigmask(SIG_BLOCK, &mask, NULL) != 0)
        return -1;
    return signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
}

int main(int argc, char **argv)
{
    char err[512] = "";
    int sig_fd;
    int rc = 0;
    int i;
    bool verbose = false;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(stdout);
            return 0;
        }
        if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
            printf("%s\n", OMACALL_VERSION);
            return 0;
        }
        if (strcmp(arg, "-v") == 0 || strcmp(arg, "--verbose") == 0) {
            verbose = true;
            continue;
        }
        if (strcmp(arg, "-c") == 0 || strcmp(arg, "--config") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "omacalld: %s needs a path\n", arg);
                return 2;
            }
            oma_strlcpy(config_path, argv[++i], sizeof(config_path));
            continue;
        }
        fprintf(stderr, "omacalld: unknown option %s\n", arg);
        usage(stderr);
        return 2;
    }

    oma_log_set_level(verbose ? OMA_LOG_DEBUG : OMA_LOG_INFO);

    if (config_path[0] == '\0'
            && oma_path_config(config_path, sizeof(config_path), "config.json") != 0) {
        oma_err("cannot resolve the configuration directory: %m");
        return 1;
    }
    if (oma_config_load(&config, config_path, err, sizeof(err)) != 0) {
        oma_err("%s", err);
        return 1;
    }
    if (!verbose)
        oma_log_set_level(config.log_level >= 4 ? OMA_LOG_DEBUG : OMA_LOG_INFO);

    if (oma_store_init() != 0) {
        oma_err("cannot open the data directory: %m");
        return 1;
    }
    if (oma_events_init() != 0) {
        oma_err("cannot create the event pipe: %m");
        oma_store_shutdown();
        return 1;
    }

    sig_fd = setup_signalfd();
    if (sig_fd < 0) {
        oma_err("cannot set up signal handling: %m");
        oma_events_shutdown();
        oma_store_shutdown();
        return 1;
    }

    rc = oma_ipc_start(err, sizeof(err));
    if (rc != 0) {
        oma_err("%s", err);
        close(sig_fd);
        oma_events_shutdown();
        oma_store_shutdown();
        /* Exit 3 says "someone else is already running", which is what lets the
         * shell's supervisor stop retrying. */
        return rc == -2 ? 3 : 1;
    }
    oma_ipc_bind_config(&config, config_path);
    oma_calls_init(&config);

    if (oma_sip_start(&config, err, sizeof(err)) != 0) {
        oma_err("%s", err);
        oma_sip_stop();
        oma_ipc_stop();
        close(sig_fd);
        oma_events_shutdown();
        oma_store_shutdown();
        return 1;
    }

    oma_info("omacalld %s ready", OMACALL_VERSION);

    while (!stopping && !oma_ipc_quit_requested()) {
        struct pollfd fds[MAX_POLL_FDS];
        size_t ipc_count = oma_ipc_pollfds(fds, MAX_POLL_FDS - 2);
        size_t total = ipc_count;
        oma_event ev;
        int ready;

        fds[total].fd = oma_events_fd();
        fds[total].events = POLLIN;
        fds[total].revents = 0;
        total++;

        fds[total].fd = sig_fd;
        fds[total].events = POLLIN;
        fds[total].revents = 0;
        total++;

        ready = poll(fds, (nfds_t)total, 1000);
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            oma_err("poll failed: %m");
            break;
        }

        if (fds[total - 1].revents & POLLIN) {
            struct signalfd_siginfo info;

            while (read(sig_fd, &info, sizeof(info)) == (ssize_t)sizeof(info))
                oma_info("caught %s, shutting down",
                         info.ssi_signo == SIGINT ? "SIGINT" : "SIGTERM");
            stopping = 1;
            break;
        }

        if (fds[total - 2].revents & POLLIN)
            oma_events_drain_pipe();

        /* Drain unconditionally: an event can be queued while the doorbell byte
         * is still in flight, and a missed wakeup would strand it for a second. */
        while (oma_events_next(&ev))
            handle_event(&ev);

        oma_ipc_dispatch(fds, ipc_count);

        /* Once a second is plenty for a timeout measured in tens of seconds. */
        oma_calls_tick();
    }

    oma_info("shutting down");
    /* Socket first. Tearing pjsua down means an un-REGISTER and a network round
     * trip, and the shell does not wait for us: disabling the plugin kills the
     * process partway through, leaving the socket file behind. Unlinking is
     * instant, so do it while we still certainly can. The un-REGISTER that
     * follows is best effort — the registration expires on its own anyway. */
    oma_ipc_stop();
    oma_calls_shutdown();
    oma_sip_stop();
    close(sig_fd);
    oma_events_shutdown();
    oma_store_shutdown();
    return 0;
}
