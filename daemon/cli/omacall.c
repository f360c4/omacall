/* omacall — command line client for omacalld.
 *
 * Opens the daemon's Unix socket, sends one NDJSON request, prints the reply
 * and exits. Keybinds and scripts use it; so do the milestone tests.
 *
 * Exit status: 0 when the daemon answered ok, 1 when it answered with an
 * error, 2 when it could not be reached. */

#include "../src/util.h"

#include <cjson/cJSON.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define REPLY_MAX (1024 * 1024)

static bool json_output;

static void usage(FILE *out)
{
    fprintf(out,
        "omacall " OMACALL_VERSION " — control omacalld\n"
        "\n"
        "Usage: omacall [--json] <command>\n"
        "\n"
        "  status                 registration and call state\n"
        "  call <number|uri>      place a call\n"
        "  answer                 answer the ringing call\n"
        "  hangup                 end or reject the current call\n"
        "  dtmf <digits>          send DTMF on the connected call\n"
        "  mute on|off            mute or unmute the microphone\n"
        "  history [--limit N]    recent calls, newest first\n"
        "  contacts               the contact list\n"
        "  config get             the configuration, without the password\n"
        "  register on|off        force a REGISTER or un-REGISTER\n"
        "  quit                   stop the daemon\n"
        "\n"
        "  --json                 print the daemon's raw reply\n");
}

static int connect_daemon(void)
{
    struct sockaddr_un addr;
    char path[512];
    int fd;

    if (oma_path_runtime(path, sizeof(path), "omacall.sock") != 0) {
        fprintf(stderr, "omacall: cannot resolve the runtime directory\n");
        return -1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (oma_strlcpy(addr.sun_path, path, sizeof(addr.sun_path)) >= sizeof(addr.sun_path)) {
        fprintf(stderr, "omacall: socket path is too long\n");
        return -1;
    }

    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        fprintf(stderr, "omacall: cannot create a socket: %s\n", strerror(errno));
        return -1;
    }
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        fprintf(stderr, "omacall: the daemon is not running (%s: %s)\n",
                path, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int send_line(int fd, const char *line)
{
    size_t len = strlen(line);

    while (len > 0) {
        ssize_t sent = write(fd, line, len);

        if (sent < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        line += sent;
        len -= (size_t)sent;
    }
    return write(fd, "\n", 1) == 1 ? 0 : -1;
}

/* Read lines until one of them carries our id. Events can arrive in between,
 * since the daemon broadcasts to every client including this one. */
static cJSON *await_reply(int fd, int want_id)
{
    char *buf = malloc(REPLY_MAX);
    size_t len = 0;

    if (buf == NULL)
        return NULL;

    for (;;) {
        char *newline;
        ssize_t got;
        size_t start = 0;

        got = read(fd, buf + len, REPLY_MAX - len - 1);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (got == 0)
            break;
        len += (size_t)got;
        buf[len] = '\0';

        while ((newline = memchr(buf + start, '\n', len - start)) != NULL) {
            size_t line_len = (size_t)(newline - (buf + start));
            cJSON *msg;

            *newline = '\0';
            msg = cJSON_Parse(buf + start);
            start += line_len + 1;

            if (msg == NULL)
                continue;
            if (cJSON_GetObjectItemCaseSensitive(msg, "id") != NULL) {
                const cJSON *id = cJSON_GetObjectItemCaseSensitive(msg, "id");

                if (cJSON_IsNumber(id) && id->valueint == want_id) {
                    free(buf);
                    return msg;
                }
            }
            cJSON_Delete(msg);
        }

        if (start > 0) {
            memmove(buf, buf + start, len - start);
            len -= start;
        }
        if (len + 1 >= REPLY_MAX)
            break;
    }

    free(buf);
    return NULL;
}

static void print_status(const cJSON *result)
{
    const cJSON *account = cJSON_GetObjectItemCaseSensitive(result, "account");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(result, "state");
    const cJSON *registered = cJSON_GetObjectItemCaseSensitive(result, "registered");
    const cJSON *uri = cJSON_GetObjectItemCaseSensitive(account, "uri");
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(account, "status_code");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(account, "status_text");

    const cJSON *call = cJSON_GetObjectItemCaseSensitive(result, "call");

    printf("state:      %s\n", cJSON_IsString(state) ? state->valuestring : "?");
    printf("registered: %s\n", cJSON_IsTrue(registered) ? "true" : "false");
    if (cJSON_IsString(uri) && uri->valuestring[0] != '\0')
        printf("account:    %s\n", uri->valuestring);
    if (cJSON_IsNumber(code) && code->valueint != 0)
        printf("registrar:  %d %s\n", code->valueint,
               cJSON_IsString(text) ? text->valuestring : "");

    if (cJSON_IsObject(call)) {
        const cJSON *number = cJSON_GetObjectItemCaseSensitive(call, "number");
        const cJSON *dir = cJSON_GetObjectItemCaseSensitive(call, "direction");
        const cJSON *call_state = cJSON_GetObjectItemCaseSensitive(call, "state");
        const cJSON *codec = cJSON_GetObjectItemCaseSensitive(call, "codec");
        const cJSON *muted = cJSON_GetObjectItemCaseSensitive(call, "muted");
        const cJSON *started = cJSON_GetObjectItemCaseSensitive(call, "started_at");

        printf("call:       %s %s (%s)\n",
               cJSON_IsString(dir) ? dir->valuestring : "?",
               cJSON_IsString(number) ? number->valuestring : "",
               cJSON_IsString(call_state) ? call_state->valuestring : "");
        if (cJSON_IsString(codec) && codec->valuestring[0] != '\0')
            printf("codec:      %s\n", codec->valuestring);
        if (cJSON_IsNumber(started) && started->valuedouble > 0) {
            int seconds = (int)(time(NULL) - (time_t)started->valuedouble);

            printf("duration:   %d:%02d\n", seconds / 60, seconds % 60);
        }
        if (cJSON_IsTrue(muted))
            printf("muted:      yes\n");
    }
}

static void print_history(const cJSON *result)
{
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(result, "items");
    const cJSON *item;

    cJSON_ArrayForEach(item, items) {
        const cJSON *ts = cJSON_GetObjectItemCaseSensitive(item, "ts");
        const cJSON *dir = cJSON_GetObjectItemCaseSensitive(item, "direction");
        const cJSON *number = cJSON_GetObjectItemCaseSensitive(item, "number");
        const cJSON *display = cJSON_GetObjectItemCaseSensitive(item, "display");
        const cJSON *reason = cJSON_GetObjectItemCaseSensitive(item, "reason");
        const cJSON *duration = cJSON_GetObjectItemCaseSensitive(item, "duration_sec");
        char stamp[32] = "";
        int seconds;

        if (cJSON_IsNumber(ts)) {
            time_t when = (time_t)ts->valuedouble;
            struct tm tm;

            if (localtime_r(&when, &tm) != NULL)
                strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M", &tm);
        }
        seconds = cJSON_IsNumber(duration) ? duration->valueint : 0;

        printf("%s  %-3s  %-16s  %-10s  %d:%02d  %s\n", stamp,
               cJSON_IsString(dir) ? dir->valuestring : "?",
               cJSON_IsString(number) ? number->valuestring : "",
               cJSON_IsString(reason) ? reason->valuestring : "",
               seconds / 60, seconds % 60,
               cJSON_IsString(display) ? display->valuestring : "");
    }
}

static void print_contacts(const cJSON *result)
{
    const cJSON *contacts = cJSON_GetObjectItemCaseSensitive(result, "contacts");
    const cJSON *item;

    cJSON_ArrayForEach(item, contacts) {
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        const cJSON *number = cJSON_GetObjectItemCaseSensitive(item, "number");
        const cJSON *id = cJSON_GetObjectItemCaseSensitive(item, "id");

        printf("%-24s  %-16s  %s\n",
               cJSON_IsString(name) ? name->valuestring : "",
               cJSON_IsString(number) ? number->valuestring : "",
               cJSON_IsString(id) ? id->valuestring : "");
    }
}

int main(int argc, char **argv)
{
    const char *command = NULL;
    cJSON *request, *args = NULL, *reply;
    const cJSON *ok, *result;
    char *line;
    int fd, rc, i;
    int limit = 50;
    const char *register_arg = NULL;
    const char *positional = NULL;

    for (i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (strcmp(arg, "--json") == 0) {
            json_output = true;
        } else if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0) {
            usage(stdout);
            return 0;
        } else if (strcmp(arg, "-V") == 0 || strcmp(arg, "--version") == 0) {
            printf("%s\n", OMACALL_VERSION);
            return 0;
        } else if (strcmp(arg, "--limit") == 0 && i + 1 < argc) {
            limit = atoi(argv[++i]);
        } else if (command == NULL) {
            command = arg;
        } else if (strcmp(command, "config") == 0 && strcmp(arg, "get") == 0) {
            command = "config.get";
        } else if (strcmp(command, "register") == 0) {
            register_arg = arg;
        } else if (positional == NULL) {
            positional = arg;
        } else {
            fprintf(stderr, "omacall: unexpected argument %s\n", arg);
            return 2;
        }
    }

    if (command == NULL) {
        usage(stderr);
        return 2;
    }
    if (strcmp(command, "config") == 0) {
        fprintf(stderr, "omacall: config needs a subcommand (get)\n");
        return 2;
    }

    if (strcmp(command, "history") == 0) {
        command = "history.list";
        args = cJSON_CreateObject();
        cJSON_AddNumberToObject(args, "limit", limit);
    } else if (strcmp(command, "contacts") == 0) {
        command = "contacts.list";
    } else if (strcmp(command, "call") == 0) {
        if (positional == NULL) {
            fprintf(stderr, "omacall: call needs a number or a sip: URI\n");
            return 2;
        }
        args = cJSON_CreateObject();
        cJSON_AddStringToObject(args, "dest", positional);
    } else if (strcmp(command, "dtmf") == 0) {
        if (positional == NULL) {
            fprintf(stderr, "omacall: dtmf needs digits\n");
            return 2;
        }
        args = cJSON_CreateObject();
        cJSON_AddStringToObject(args, "digits", positional);
    } else if (strcmp(command, "mute") == 0) {
        if (positional == NULL
                || (strcmp(positional, "on") != 0 && strcmp(positional, "off") != 0)) {
            fprintf(stderr, "omacall: mute needs on or off\n");
            return 2;
        }
        args = cJSON_CreateObject();
        cJSON_AddBoolToObject(args, "enabled", strcmp(positional, "on") == 0);
    } else if (strcmp(command, "register") == 0) {
        if (register_arg == NULL
                || (strcmp(register_arg, "on") != 0 && strcmp(register_arg, "off") != 0)) {
            fprintf(stderr, "omacall: register needs on or off\n");
            return 2;
        }
        args = cJSON_CreateObject();
        cJSON_AddBoolToObject(args, "enabled", strcmp(register_arg, "on") == 0);
    }

    fd = connect_daemon();
    if (fd < 0)
        return 2;

    request = cJSON_CreateObject();
    cJSON_AddNumberToObject(request, "id", 1);
    cJSON_AddStringToObject(request, "cmd", command);
    if (args != NULL)
        cJSON_AddItemToObject(request, "args", args);
    line = cJSON_PrintUnformatted(request);
    cJSON_Delete(request);

    if (line == NULL || send_line(fd, line) != 0) {
        fprintf(stderr, "omacall: cannot send the request\n");
        free(line);
        close(fd);
        return 2;
    }
    free(line);

    reply = await_reply(fd, 1);
    close(fd);
    if (reply == NULL) {
        fprintf(stderr, "omacall: no reply from the daemon\n");
        return 2;
    }

    ok = cJSON_GetObjectItemCaseSensitive(reply, "ok");
    result = cJSON_GetObjectItemCaseSensitive(reply, "result");

    if (json_output) {
        char *text = cJSON_Print(reply);

        if (text != NULL) {
            printf("%s\n", text);
            free(text);
        }
    } else if (!cJSON_IsTrue(ok)) {
        const cJSON *error = cJSON_GetObjectItemCaseSensitive(reply, "error");
        const cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(error, "message");

        fprintf(stderr, "omacall: %s: %s\n",
                cJSON_IsString(code) ? code->valuestring : "error",
                cJSON_IsString(message) ? message->valuestring : "");
    } else if (strcmp(command, "status") == 0) {
        print_status(result);
    } else if (strcmp(command, "history.list") == 0) {
        print_history(result);
    } else if (strcmp(command, "contacts.list") == 0) {
        print_contacts(result);
    } else if (strcmp(command, "config.get") == 0) {
        char *text = cJSON_Print(result);

        if (text != NULL) {
            printf("%s\n", text);
            free(text);
        }
    } else {
        printf("ok\n");
    }

    rc = cJSON_IsTrue(ok) ? 0 : 1;
    cJSON_Delete(reply);
    return rc;
}
