/*
 * (C) Copyright 2008-2017
 * Stefano Babic, DENX Software Engineering, sbabic@denx.de.
 * 	on behalf of ifm electronic GmbH
 *
 * SPDX-License-Identifier:     LGPL-2.1-or-later
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/un.h>
#include "network_ipc.h"
#include "compat.h"

#ifdef CONFIG_SOCKET_CTRL_PATH
static char* SOCKET_CTRL_PATH = (char*)CONFIG_SOCKET_CTRL_PATH;
#else
static char* SOCKET_CTRL_PATH = NULL;
#endif

#define SOCKET_CTRL_DEFAULT  "sockinstctrl"

char *get_ctrl_socket(void) {
	if (!SOCKET_CTRL_PATH || !strlen(SOCKET_CTRL_PATH)) {
		const char *tmpdir = getenv("TMPDIR");
		if (!tmpdir)
			tmpdir = "/tmp";

		if (asprintf(&SOCKET_CTRL_PATH, "%s/%s", tmpdir, SOCKET_CTRL_DEFAULT) == -1)
			return (char *)"/tmp/"SOCKET_CTRL_DEFAULT;
	}

	return SOCKET_CTRL_PATH;
}

static int prepare_ipc(void) {
	int connfd;
	struct sockaddr_un servaddr;

	connfd = socket(AF_LOCAL, SOCK_STREAM, 0);
	if (connfd < 0)
		return -1;

	bzero(&servaddr, sizeof(servaddr));
	servaddr.sun_family = AF_LOCAL;

	strncpy(servaddr.sun_path, get_ctrl_socket(), sizeof(servaddr.sun_path) - 1);

	if (connect(connfd, (struct sockaddr *) &servaddr, sizeof(servaddr)) < 0) {
		close(connfd);
		return -1;
	}

	return connfd;
}

int ipc_postupdate(ipc_message *msg) {
	int connfd = prepare_ipc();
	if (connfd < 0)
		return -1;

	char* tmpbuf = NULL;
	if (msg->data.procmsg.len > 0) {
		if ((tmpbuf = strndupa(msg->data.procmsg.buf,
				msg->data.procmsg.len > sizeof(msg->data.procmsg.buf)
				    ? sizeof(msg->data.procmsg.buf)
				    : msg->data.procmsg.len)) == NULL) {
			close(connfd);
			return -1;
		}
	}

	memset(msg, 0, sizeof(*msg));
	if (tmpbuf != NULL) {
		msg->data.procmsg.buf[sizeof(msg->data.procmsg.buf) - 1] = '\0';
		strncpy(msg->data.procmsg.buf, tmpbuf, sizeof(msg->data.procmsg.buf) - 1);
		msg->data.procmsg.len = strnlen(tmpbuf, sizeof(msg->data.procmsg.buf) - 1);
	}
	msg->magic = IPC_MAGIC;
	msg->type = POST_UPDATE;

	int result = write(connfd, msg, sizeof(*msg)) != sizeof(*msg) ||
		read(connfd, msg, sizeof(*msg)) != sizeof(*msg);

	close(connfd);
	return -result;
}

static int __ipc_get_status(int connfd, ipc_message *msg, unsigned int timeout_ms)
{
	fd_set fds;
	struct timeval tv;

	memset(msg, 0, sizeof(*msg));
	msg->magic = IPC_MAGIC;
	msg->type = GET_STATUS;

	if (write(connfd, msg, sizeof(*msg)) != sizeof(*msg))
		return -1;

	if (timeout_ms) {
		FD_ZERO(&fds);
		FD_SET(connfd, &fds);

		/*
		 * Invalid the message
		 * Caller should check it
		 */
		msg->magic = 0;

		tv.tv_sec = 0;
		tv.tv_usec = timeout_ms * 1000;
		if ((select(connfd + 1, &fds, NULL, NULL, &tv) <= 0) ||
			!FD_ISSET(connfd, &fds))
			return -ETIMEDOUT;
	}

	return -(read(connfd, msg, sizeof(*msg)) != sizeof(*msg));
}

int ipc_get_status(ipc_message *msg)
{
	int ret;
	int connfd;

	connfd = prepare_ipc();
	if (connfd < 0)
		return -1;

	ret = __ipc_get_status(connfd, msg, 0);
	close(connfd);

	return ret;
}

/*
 * @return : 0  : TIMEOUT
 *           -1 : error
 *           else data read
 */
int ipc_get_status_timeout(ipc_message *msg, unsigned int timeout_ms)
{
	int ret;
	int connfd;

	connfd = prepare_ipc();
	if (connfd < 0)
		return -1;

	ret = __ipc_get_status(connfd, msg, timeout_ms);
	close(connfd);

	/* Not very nice, but necessary in order to keep the API consistent. */
	if (timeout_ms && ret == -ETIMEDOUT)
		return 0;

	return ret == 0 ? sizeof(*msg) : -1;
}

int ipc_inst_start_ext(void *priv, ssize_t size)
{
	int connfd;
	ipc_message msg;
	struct swupdate_request *req;
	struct swupdate_request localreq;

	if (priv) {
		/*
		 * To be expanded: in future if more API will
		 * be supported, a conversion will be take place
		 * to send to the installer a single identifiable
		 * request
		 */
		if (size != sizeof(struct swupdate_request))
			return -EINVAL;
		req = (struct swupdate_request *)priv;
	} else {
		/*
		 * ensure that a valid install request
		 * reaches the installer, add an empty
		 * one with default values
		 */
		swupdate_prepare_req(&localreq);
		req = &localreq;
	}
	connfd = prepare_ipc();
	if (connfd < 0)
		return -1;

	memset(&msg, 0, sizeof(msg));

	/*
	 * Command is request to install
	 */
	msg.magic = IPC_MAGIC;
	msg.type = REQ_INSTALL;

	msg.data.instmsg.req = *req;
	if (write(connfd, &msg, sizeof(msg)) != sizeof(msg) ||
		read(connfd, &msg, sizeof(msg)) != sizeof(msg) ||
		msg.type != ACK)
		goto cleanup;

	return connfd;

cleanup:
	close(connfd);
	return -1;
}

/*
 * this is for compatibiity to not break external API
 * Use better the _ext() version
 */
int ipc_inst_start(void)
{
	return ipc_inst_start_ext(NULL, 0);
}

/*
 * This is not required, it is really a wrapper for
 * write, but make interface consistent
 */
int ipc_send_data(int connfd, char *buf, int size)
{
	ssize_t ret = write(connfd, buf, (size_t)size);
	return ret != size ? -1 : (int)ret;
}

void ipc_end(int connfd)
{
	close(connfd);
}

int ipc_wait_for_complete(getstatus callback)
{
	int fd;
	RECOVERY_STATUS status = IDLE;
	ipc_message message;
	int ret;
	
	message.data.status.last_result = FAILURE;

	do {
		fd = prepare_ipc();
		if (fd < 0)
			break;
		ret = __ipc_get_status(fd, &message, 0);
		close(fd);

		if (ret < 0) {
			message.data.status.last_result = FAILURE;
			break;
		}

		if (( (status != (RECOVERY_STATUS)message.data.status.current) ||
			strlen(message.data.status.desc))) {
				if (callback)
					callback(&message);
			} else
				sleep(1);

		status = (RECOVERY_STATUS)message.data.status.current;
	} while(message.data.status.current != IDLE);

	return message.data.status.last_result;
}

int ipc_send_cmd(ipc_message *msg, int timeout_ms)
{
	int connfd = prepare_ipc();
	int ret;
    fd_set fds;
	struct timeval tv;

	if (connfd < 0) {
		return -1;
	}

	/* TODO: Check source type */
	msg->magic = IPC_MAGIC;
	ret = write(connfd, msg, sizeof(*msg));
	if (ret != sizeof(*msg)) {
		close(connfd);
		return -1;
	}

    if (!timeout_ms) {
		ret = read(connfd, msg, sizeof(*msg));
	} else {
		FD_ZERO(&fds);
		FD_SET(connfd, &fds);
		tv.tv_sec = 0;
		tv.tv_usec = timeout_ms * 1000;
		ret = select(connfd + 1, &fds, NULL, NULL, &tv);
		if (ret <= 0 || !FD_ISSET(connfd, &fds))
			return 0;
		ret = read(connfd, msg, sizeof(*msg));
	}
	close(connfd);

	return 0;
}
