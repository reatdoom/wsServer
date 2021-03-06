/*
 * Copyright (C) 2016-2020  Davidson Francis <davidsondfgl@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>
 */
#include <arpa/inet.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <ws.h>

/**
 * @dir src/
 * @brief wsServer source code
 *
 * @file ws.c
 * @brief wsServer main routines.
 */

/**
 * @brief Opened ports.
 */
int port_index;

/**
 * @brief Port entry in @ref ws_port structure.
 *
 * This defines the port number and events for a single
 * call to @ref ws_socket. This allows that multiples threads
 * can call @ref ws_socket, configuring different ports and
 * events for each call.
 */
struct ws_port
{
	int port_number;         /**< Port number.      */
	struct ws_events events; /**< Websocket events. */
};

/**
 * @brief Ports list.
 */
struct ws_port ports[MAX_PORTS];

/**
 * @brief Client socks.
 */
struct ws_connection
{
	int client_sock; /**< Client socket FD.        */
	int port_index;  /**< Index in the port list.  */
};

/**
 * @brief Clients list.
 */
struct ws_connection client_socks[MAX_CLIENTS];

/**
 * @brief WebSocket frame data
 */
struct ws_frame_data
{
	/**
	 * @brief Frame read.
	 */
	unsigned char frm[MESSAGE_LENGTH];
	/**
	 * @brief Processed message at the moment.
	 */
	unsigned char *msg;
	/**
	 * @brief Current byte position.
	 */
	size_t cur_pos;
	/**
	 * @brief Amount of read bytes.
	 */
	size_t amt_read;
	/**
	 * @brief Frame type, like text or binary.
	 */
	int frame_type;
	/**
	 * @brief Frame size.
	 */
	size_t frame_size;
	/**
	 * @brief Error flag, set when a read was not possible.
	 */
	int error;
	/**
	 * @brief Client socket file descriptor.
	 */
	int sock;
};

/**
 * @brief Global mutex.
 */
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/**
 * @brief Issues an error message and aborts the program.
 *
 * @param s Error message.
 */
#define panic(s)   \
	do             \
	{              \
		perror(s); \
		exit(-1);  \
	} while (0);

/**
 * @brief Gets the IP address relative to a file descriptor opened
 * by the server.
 *
 * @param fd File descriptor target.
 *
 * @return Pointer the ip address.
 */
char *ws_getaddress(int fd)
{
	struct sockaddr_in addr;
	socklen_t addr_size;
	char *client;

	addr_size = sizeof(struct sockaddr_in);
	if (getpeername(fd, (struct sockaddr *)&addr, &addr_size) < 0)
		return NULL;

	client = malloc(sizeof(char) * 20);
	strcpy(client, inet_ntoa(addr.sin_addr));
	return (client);
}

/**
 * @brief Creates and send an WebSocket frame with some payload data.
 *
 * This routine is intended to be used to create a websocket frame for
 * a given type e sending to the client. For higher level routines,
 * please check @ref ws_sendframe_txt and @ref ws_sendframe_bin.
 *
 * @param fd        Target to be send.
 * @param msg       Message to be send.
 * @param size      Binary message size (set it <0 for text message)
 * @param broadcast Enable/disable broadcast.
 * @param type      Frame type.
 *
 * @return Returns the number of bytes written.
 *
 * @note If @p size is -1, it is assumed that a text frame is being sent,
 * otherwise, a binary frame. In the later case, the @p size is used.
 */
int ws_sendframe(int fd, const char *msg, ssize_t size, bool broadcast, int type)
{
	unsigned char *response; /* Response data.     */
	unsigned char frame[10]; /* Frame.             */
	uint8_t idx_first_rData; /* Index data.        */
	uint64_t length;         /* Message length.    */
	int idx_response;        /* Index response.    */
	ssize_t output;          /* Bytes sent.        */
	int sock;                /* File Descript.     */
	uint64_t i;              /* Loop index.        */
	int cur_port_index;      /* Current port index */

	frame[0] = (WS_FIN | type);
	length   = size;

	/* Guess the size if not informed, perhaps a TXT frame. */
	if (size < 0)
		length = strlen((const char *)msg);

	/* Split the size between octects. */
	if (length <= 125)
	{
		frame[1]        = length & 0x7F;
		idx_first_rData = 2;
	}

	/* Size between 126 and 65535 bytes. */
	else if (length >= 126 && length <= 65535)
	{
		frame[1]        = 126;
		frame[2]        = (length >> 8) & 255;
		frame[3]        = length & 255;
		idx_first_rData = 4;
	}

	/* More than 65535 bytes. */
	else
	{
		frame[1]        = 127;
		frame[2]        = (unsigned char)((length >> 56) & 255);
		frame[3]        = (unsigned char)((length >> 48) & 255);
		frame[4]        = (unsigned char)((length >> 40) & 255);
		frame[5]        = (unsigned char)((length >> 32) & 255);
		frame[6]        = (unsigned char)((length >> 24) & 255);
		frame[7]        = (unsigned char)((length >> 16) & 255);
		frame[8]        = (unsigned char)((length >> 8) & 255);
		frame[9]        = (unsigned char)(length & 255);
		idx_first_rData = 10;
	}

	/* Add frame bytes. */
	idx_response = 0;
	response     = malloc(sizeof(unsigned char) * (idx_first_rData + length + 1));
	for (i = 0; i < idx_first_rData; i++)
	{
		response[i] = frame[i];
		idx_response++;
	}

	/* Add data bytes. */
	for (i = 0; i < length; i++)
	{
		response[idx_response] = msg[i];
		idx_response++;
	}

	response[idx_response] = '\0';
	output                 = write(fd, response, idx_response);
	if (broadcast)
	{
		pthread_mutex_lock(&mutex);
		cur_port_index = -1;
		for (i = 0; i < MAX_CLIENTS; i++)
			if (client_socks[i].client_sock == fd)
				cur_port_index = client_socks[i].port_index, i = MAX_CLIENTS;

		for (i = 0; i < MAX_CLIENTS; i++)
		{
			sock = client_socks[i].client_sock;
			if ((sock > -1) && (sock != fd) &&
				(client_socks[i].port_index == cur_port_index))
				output += write(sock, response, idx_response);
		}
		pthread_mutex_unlock(&mutex);
	}

	free(response);
	return ((int)output);
}

/**
 * @brief Sends a WebSocket text frame.
 *
 * @param fd         Target to be send.
 * @param msg        Text message to be send.
 * @param broadcast  Enable/disable broadcast (0-disable/anything-enable).
 *
 * @return Returns the number of bytes written.
 */
int ws_sendframe_txt(int fd, const char *msg, bool broadcast)
{
	return ws_sendframe(fd, msg, -1, broadcast, WS_FR_OP_TXT);
}

/**
 * @brief Sends a WebSocket text frame.
 *
 * @param fd         Target to be send.
 * @param msg        Binary message to be send.
 * @param size       Message size (in bytes).
 * @param broadcast  Enable/disable broadcast (0-disable/anything-enable).
 *
 * @return Returns the number of bytes written.
 */
int ws_sendframe_bin(int fd, const char *msg, size_t size, bool broadcast)
{
	return ws_sendframe(fd, msg, size, broadcast, WS_FR_OP_BIN);
}

/**
 * @brief Do the handshake process.
 *
 * @param wfd Websocket Frame Data.
 * @param p_index Client port index.
 *
 * @return Returns 0 if success, a negative number otherwise.
 *
 * @attention This is part of the internal API and is documented just
 * for completeness.
 */
static int do_handshake(struct ws_frame_data *wfd, int p_index)
{
	char *response; /* Handshake response message. */
	char *p;        /* Last request line pointer.  */
	ssize_t n;      /* Read/Write bytes.           */

	/* Read the very first client message. */
	if ((n = read(wfd->sock, wfd->frm, sizeof(wfd->frm) - 1)) < 0)
		return (-1);

	/* Advance our pointers before the first next_byte(). */
	p = strstr((const char *)wfd->frm, "\r\n\r\n");
	if (p == NULL)
	{
		DEBUG("An empty line with \\r\\n was expected!\n");
		return (-1);
	}
	wfd->amt_read = n;
	wfd->cur_pos  = (size_t)((ptrdiff_t)(p - (char *)wfd->frm)) + 4;

	/* Get response. */
	if (get_handshake_response((char *)wfd->frm, &response) < 0)
	{
		DEBUG("Cannot get handshake response, request was: %s\n", wfd->frm);
		return (-1);
	}

	/* Valid request. */
	DEBUG("Handshaked, response: \n"
		  "------------------------------------\n"
		  "%s"
		  "------------------------------------\n",
		response);

	/* Send handshake. */
	if (write(wfd->sock, response, strlen(response)) < 0)
	{
		DEBUG("As error has ocurred while handshaking!\n");
		return (-1);
	}

	/* Trigger events and clean up buffers. */
	ports[p_index].events.onopen(wfd->sock);
	free(response);
	return (0);
}

/**
 * @brief Send a pong frame in response to a ping frame.
 *
 * Accordingly to the RFC, a pong frame must have the same
 * data payload as the ping frame, so we just send a
 * ordinary frame with PONG opcode.
 *
 * @param wfd Websocket frame data.
 *
 * @return Returns 0 if success and a negative number
 * otherwise.
 *
 * @attention This is part of the internal API and is documented just
 * for completeness.
 */
static int do_pong(struct ws_frame_data *wfd)
{
	if (ws_sendframe(wfd->sock, (const char *)wfd->msg, wfd->frame_size, false,
			WS_FR_OP_PONG) < 0)
	{
		DEBUG("An error has ocurred while ponging!\n");
		free(wfd->msg);
		return (-1);
	}
	free(wfd->msg);
	return (0);
}

/**
 * @brief Read a chunk of bytes and return the next byte
 * belonging to the frame.
 *
 * @param wfd Websocket Frame Data.
 *
 * @return Returns the byte read, or -1 if error.
 *
 * @attention This is part of the internal API and is documented just
 * for completeness.
 */
static inline int next_byte(struct ws_frame_data *wfd)
{
	ssize_t n;

	/* If empty or full. */
	if (wfd->cur_pos == 0 || wfd->cur_pos == wfd->amt_read)
	{
		if ((n = read(wfd->sock, wfd->frm, sizeof(wfd->frm))) <= 0)
		{
			wfd->error = 1;
			DEBUG("An error has ocorred while trying to read next byte\n");
			return (-1);
		}
		wfd->amt_read = (size_t)n;
		wfd->cur_pos  = 0;
	}
	return (wfd->frm[wfd->cur_pos++]);
}

/**
 * @brief Reads the next frame, whether if a TXT/BIN/CLOSE
 * of arbitrary size.
 *
 * @param wfd Websocket Frame Data.
 *
 * @return Returns 0 if success, a negative number otherwise.
 *
 * @attention This is part of the internal API and is documented just
 * for completeness.
 */
static int next_frame(struct ws_frame_data *wfd)
{
	size_t frame_length; /* Frame length.       */
	unsigned char *msg;  /* Message.            */
	uint8_t masks[4];    /* Masks array.        */
	size_t msg_idx;      /* Current msg index.  */
	uint8_t opcode;      /* Frame opcode.       */
	char cur_byte;       /* Current frame byte. */
	uint8_t mask;        /* Mask.               */
	uint8_t is_fin;      /* Is FIN frame flag.  */
	size_t i;            /* Loop index.         */

	msg             = NULL;
	is_fin          = 0;
	msg_idx         = 0;
	wfd->frame_size = 0;

	/* Read until find a FIN or a unsupported frame. */
	do
	{
		/*
		 * Obs: next_byte() can return error if not possible to read the
		 * next frame byte, in this case, we return an error.
		 *
		 * However, please note that this check is only made here and in
		 * the subsequent next_bytes() calls this also may occur too.
		 * wsServer is assuming that the client only create right
		 * frames and we will do not have disconnections while reading
		 * the frame but just when waiting for a frame.
		 */
		cur_byte = next_byte(wfd);
		if (cur_byte == -1)
			return (-1);

		is_fin = (cur_byte & 0xFF) >> WS_FIN_SHIFT;
		opcode = (cur_byte & 0xF);

		if (opcode == WS_FR_OP_TXT || opcode == WS_FR_OP_BIN ||
			opcode == WS_FR_OP_CONT || opcode == WS_FR_OP_PING)
		{
			/* Only change frame type if not a CONT frame. */
			if (opcode != WS_FR_OP_CONT)
				wfd->frame_type = opcode;

			mask         = next_byte(wfd);
			frame_length = mask & 0x7F;

			/* Decode masks and length for 16-bit messages. */
			if (frame_length == 126)
				frame_length = (((size_t)next_byte(wfd)) << 8) | next_byte(wfd);

			/* 64-bit messages. */
			else if (frame_length == 127)
			{
				frame_length = (((size_t)next_byte(wfd)) << 56) | /* frame[2]. */
							   (((size_t)next_byte(wfd)) << 48) | /* frame[3]. */
							   (((size_t)next_byte(wfd)) << 40) |
							   (((size_t)next_byte(wfd)) << 32) |
							   (((size_t)next_byte(wfd)) << 24) |
							   (((size_t)next_byte(wfd)) << 16) |
							   (((size_t)next_byte(wfd)) << 8) |
							   (((size_t)next_byte(wfd))); /* frame[9]. */
			}

			wfd->frame_size += frame_length;

			/* Read masks. */
			masks[0] = next_byte(wfd);
			masks[1] = next_byte(wfd);
			masks[2] = next_byte(wfd);
			masks[3] = next_byte(wfd);

			/*
			 * Abort if error.
			 *
			 * This is tricky: we may have multiples error codes from the
			 * previous next_bytes() calls, but, since we're only setting
			 * variables and flags, there is no major issue in setting
			 * them wrong _if_ we do not use their values, thing that
			 * we do here.
			 */
			if (wfd->error)
				break;

			/*
			 * Allocate memory.
			 *
			 * The statement below will allocate a new chunk of memory
			 * if msg is NULL with size total_length. Otherwise, it will
			 * resize the total memory accordingly with the message index
			 * and if the current frame is a FIN frame or not, if so,
			 * increment the size by 1 to accomodate the line ending \0.
			 */
			msg = realloc(
				msg, sizeof(unsigned char) * (msg_idx + frame_length + is_fin));

			/* Copy to the proper location. */
			for (i = 0; i < frame_length; i++, msg_idx++)
			{
				/* We were able to read? .*/
				cur_byte = next_byte(wfd);
				if (cur_byte == -1)
					goto abort;

				msg[msg_idx] = cur_byte ^ masks[i % 4];
			}

			/* If we're inside a FIN frame, lets... */
			if (is_fin)
				msg[msg_idx] = '\0';
		}

		/* Anything else (close frame or unsupported). */
		else
			wfd->frame_type = opcode;

	} while (!is_fin && !wfd->error);

abort:
	/* Check for error. */
	if (wfd->error)
	{
		free(msg);
		wfd->msg = NULL;
		return (-1);
	}

	wfd->msg = msg;
	return (0);
}

/**
 * @brief Establishes to connection with the client and trigger
 * events when occurs one.
 *
 * @param vsock Client connection index.
 *
 * @return Returns @p vsock if success and a negative
 * number otherwise.
 *
 * @note This will be run on a different thread.
 *
 * @attention This is part of the internal API and is documented just
 * for completeness.
 */
static void *ws_establishconnection(void *vsock)
{
	struct ws_frame_data wfd; /* WebSocket frame data.   */
	int connection_index;     /* Client connect. index.  */
	int close_frame;          /* Close frame flag.       */
	int p_index;              /* Port list index.        */
	int sock;                 /* File descriptor.        */
	int i;                    /* Loop index.             */

	close_frame      = 0;
	connection_index = (int)(intptr_t)vsock;
	sock             = client_socks[connection_index].client_sock;
	p_index          = client_socks[connection_index].port_index;

	/* Prepare frame data. */
	memset(&wfd, 0, sizeof(wfd));
	wfd.sock = sock;

	/* Do handshake. */
	if (do_handshake(&wfd, p_index) < 0)
		goto closed;

	/* Read next frame until client disconnects or an error occur. */
	while (next_frame(&wfd) >= 0)
	{
		/* Frame without data payload. */
		if (wfd.msg == NULL)
		{
			DEBUG("Non text frame received from %d", sock);
			if (wfd.frame_type == WS_FR_OP_CLSE)
				DEBUG(": close frame!\n");
			else
			{
				DEBUG(", type: %x\n", wfd.frame_type);
				continue;
			}
		}

		/* Text/binary event. */
		if ((wfd.frame_type == WS_FR_OP_TXT || wfd.frame_type == WS_FR_OP_BIN) &&
			!wfd.error)
		{
			ports[p_index].events.onmessage(sock, wfd.msg, wfd.frame_size);
			free(wfd.msg);
			wfd.msg = NULL;
		}

		/* Close event. */
		else if (wfd.frame_type == WS_FR_OP_CLSE && !wfd.error)
		{
			close_frame = 1;
			free(wfd.msg);
			ports[p_index].events.onclose(sock);
			break;
		}

		/* Ping. */
		else if (wfd.frame_type == WS_FR_OP_PING && !wfd.error)
			if (do_pong(&wfd) < 0)
				break;
	}

	/*
	 * If we do not receive a close frame, we still need to
	 * call the close event, as the server is expected to
	 * always know when the client disconnects.
	 */
	if (!close_frame)
		ports[p_index].events.onclose(sock);

closed:
	/* Removes client socket from socks list. */
	pthread_mutex_lock(&mutex);
	for (i = 0; i < MAX_CLIENTS; i++)
	{
		if (client_socks[i].client_sock == sock)
		{
			client_socks[i].client_sock = -1;
			break;
		}
	}
	pthread_mutex_unlock(&mutex);

	close(sock);
	return (vsock);
}

/**
 * @brief Main loop for the server.
 *
 * @param evs  Events structure.
 * @param port Server port.
 *
 * @return This function never returns.
 *
 * @note Note that this function can be called multiples times,
 * from multiples different threads (depending on the @ref MAX_PORTS)
 * value. Each call _should_ have a different port and can have
 * differents events configured.
 */
int ws_socket(struct ws_events *evs, uint16_t port)
{
	int sock;                  /* Current socket.        */
	int new_sock;              /* New opened connection. */
	struct sockaddr_in server; /* Server.                */
	struct sockaddr_in client; /* Client.                */
	int len;                   /* Length of sockaddr.    */
	pthread_t client_thread;   /* Client thread.         */
	int i;                     /* Loop index.            */
	int connection_index;
	int p_index;

	connection_index = 0;

	/* Checks if the event list is a valid pointer. */
	if (evs == NULL)
		panic("Invalid event list!");

	pthread_mutex_lock(&mutex);
	if (port_index >= MAX_PORTS)
	{
		pthread_mutex_unlock(&mutex);
		panic("too much websocket ports opened !");
	}
	p_index = port_index;
	port_index++;
	pthread_mutex_unlock(&mutex);

	/* Copy events. */
	memcpy(&ports[p_index].events, evs, sizeof(struct ws_events));
	ports[p_index].port_number = port;

	/* Create socket. */
	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0)
		panic("Could not create socket");

	/* Reuse previous address. */
	if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int)) < 0)
		panic("setsockopt(SO_REUSEADDR) failed");

	/* Prepare the sockaddr_in structure. */
	server.sin_family      = AF_INET;
	server.sin_addr.s_addr = INADDR_ANY;
	server.sin_port        = htons(port);

	/* Bind. */
	if (bind(sock, (struct sockaddr *)&server, sizeof(server)) < 0)
		panic("Bind failed");

	/* Listen. */
	listen(sock, MAX_CLIENTS);

	/* Wait for incoming connections. */
	printf("Waiting for incoming connections...\n");

	len = sizeof(struct sockaddr_in);
	memset(client_socks, -1, sizeof(client_socks));

	/* Accept connections. */
	while (1)
	{
		/* Accept. */
		new_sock = accept(sock, (struct sockaddr *)&client, (socklen_t *)&len);
		if (new_sock < 0)
			panic("Error on accepting connections..");

		/* Adds client socket to socks list. */
		pthread_mutex_lock(&mutex);
		for (i = 0; i < MAX_CLIENTS; i++)
		{
			if (client_socks[i].client_sock == -1)
			{
				client_socks[i].client_sock = new_sock;
				client_socks[i].port_index  = p_index;
				connection_index            = i;
				break;
			}
		}
		pthread_mutex_unlock(&mutex);

		if (pthread_create(&client_thread, NULL, ws_establishconnection,
				(void *)(intptr_t)connection_index) < 0)
			panic("Could not create the client thread!");

		pthread_detach(client_thread);
	}
	return (0);
}
