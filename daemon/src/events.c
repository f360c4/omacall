#include "events.h"

#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#define QUEUE_SIZE 128

static oma_event queue[QUEUE_SIZE];
static size_t head;   /* next slot to read */
static size_t count;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

static int wake_fds[2] = { -1, -1 };

int oma_events_init(void)
{
    if (pipe2(wake_fds, O_NONBLOCK | O_CLOEXEC) != 0)
        return -1;
    head = 0;
    count = 0;
    return 0;
}

void oma_events_shutdown(void)
{
    int i;

    for (i = 0; i < 2; i++) {
        if (wake_fds[i] >= 0) {
            close(wake_fds[i]);
            wake_fds[i] = -1;
        }
    }
}

int oma_events_fd(void)
{
    return wake_fds[0];
}

void oma_events_post(const oma_event *ev)
{
    bool dropped = false;
    const unsigned char byte = 1;
    ssize_t rc;

    pthread_mutex_lock(&lock);
    if (count == QUEUE_SIZE) {
        dropped = true;
    } else {
        queue[(head + count) % QUEUE_SIZE] = *ev;
        count++;
    }
    pthread_mutex_unlock(&lock);

    if (dropped) {
        oma_warn("events: queue full, dropped event type %d", (int)ev->type);
        return;
    }

    /* The pipe is only a doorbell. If it is full the main loop already has a
     * wakeup pending and will drain the whole queue anyway, so EAGAIN is fine. */
    do {
        rc = write(wake_fds[1], &byte, 1);
    } while (rc < 0 && errno == EINTR);
}

int oma_events_next(oma_event *ev)
{
    int got = 0;

    pthread_mutex_lock(&lock);
    if (count > 0) {
        *ev = queue[head];
        head = (head + 1) % QUEUE_SIZE;
        count--;
        got = 1;
    }
    pthread_mutex_unlock(&lock);
    return got;
}

void oma_events_drain_pipe(void)
{
    char buf[256];
    ssize_t got;

    for (;;) {
        got = read(wake_fds[0], buf, sizeof(buf));
        if (got > 0)
            continue;
        if (got < 0 && errno == EINTR)
            continue;
        break;
    }
}
