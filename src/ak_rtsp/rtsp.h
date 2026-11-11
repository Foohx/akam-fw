#pragma once
#include <stdbool.h>
#include <sys/socket.h>
#include "rtsp_auth.h"

/* Accept a client connection and run the full RTSP session loop until the
 * client disconnects or teardown is received. Blocks until session ends. */
/* Initialise the process-wide credentials state; main() calls it once before any thread starts. Idempotent. */
void rtsp_auth_shared_init(void);
/* Refresh the credential state shown as rtsp.auth; force = now, else at most every 5 s. */
void rtsp_auth_tick(bool force);

/* The RTSP server's credentials state (see rtsp_auth.h), initialised. */
rtsp_auth_db_t *rtsp_auth_shared_db(void);
/* Listening socket of main(): while a session runs, a second client is answered "503" and closed (rtsp_busy.h). */
void rtsp_set_listen_fd(int fd);
void handle_client(int client_fd, struct sockaddr_storage *client_addr,
                   socklen_t addr_len);

/* Drain every frame currently ready in the VENC ring buffer (bounded)
 * without sending it. Call it periodically (after a poll() timeout) while
 * waiting in accept() so the ring doesn't wrap and the next client starts
 * near an I-frame. Do not call it in a tight loop: it does not sleep. */
void drain_venc_ring(void);
