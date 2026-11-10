#pragma once
#include <stdbool.h>
#include <sys/socket.h>

/* Accept a client connection and run the full RTSP session loop until the
 * client disconnects or teardown is received. Blocks until session ends. */
/* Refresh the credential state shown as rtsp.auth; force = now, else at most every 5 s. */
void rtsp_auth_tick(bool force);
void handle_client(int client_fd, struct sockaddr_storage *client_addr,
                   socklen_t addr_len);

/* Drain every frame currently ready in the VENC ring buffer (bounded)
 * without sending it. Call it periodically (after a poll() timeout) while
 * waiting in accept() so the ring doesn't wrap and the next client starts
 * near an I-frame. Do not call it in a tight loop: it does not sleep. */
void drain_venc_ring(void);
