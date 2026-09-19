#ifndef OMA_IPC_H
#define OMA_IPC_H

#include "config.h"
#include "events.h"

#include <cjson/cJSON.h>
#include <poll.h>
#include <stdbool.h>
#include <stddef.h>

/* The Unix socket the panel and the CLI talk to, speaking NDJSON: one JSON
 * object per line, in both directions. The contract is docs/protocol.md.
 *
 * Several clients can be connected at once. Replies go only to the client that
 * asked; events go to everyone. */

#define OMA_PROTOCOL_VERSION 1

/* Bind $XDG_RUNTIME_DIR/omacall.sock, mode 0600. Returns:
 *   0  listening
 *  -1  failed, message in `err`
 *  -2  another daemon already owns the socket */
int  oma_ipc_start(char *err, size_t errlen);
void oma_ipc_stop(void);

/* Add this module's descriptors to a pollfd array. Returns how many were
 * appended; `cap` is the room left. */
size_t oma_ipc_pollfds(struct pollfd *fds, size_t cap);

/* Handle whatever poll() reported on the descriptors from oma_ipc_pollfds. */
void oma_ipc_dispatch(const struct pollfd *fds, size_t count);

/* Broadcast an event line to every connected client. */
void oma_ipc_broadcast(const char *event, cJSON *data);

/* The daemon's live config, shared with the command handlers. */
void oma_ipc_bind_config(oma_config *cfg, const char *config_path);

/* Set by the `quit` command. */
bool oma_ipc_quit_requested(void);

#endif /* OMA_IPC_H */
