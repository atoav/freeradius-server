/*
 *   This program is is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or (at
 *   your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 * @file rlm_radius_udp.c
 * @brief RADIUS UDP transport
 *
 * @copyright 2017 Network RADIUS SAS
 * @copyright 2020 Arran Cudbard-Bell (a.cudbardb@freeradius.org)
 */

#include <freeradius-devel/io/application.h>
#include <freeradius-devel/io/listen.h>
#include <freeradius-devel/io/pair.h>
#include <freeradius-devel/missing.h>
#include <freeradius-devel/server/connection.h>
#include <freeradius-devel/util/debug.h>
#include <freeradius-devel/util/heap.h>

#include <sys/socket.h>

//#include "rlm_radius.h"
#include "track.h"

/*
 * Macro to simplify checking packets before calling decode(), so that
 * it gets a known valid length and no longer calls fr_radius_ok() itself.
 */
#define check(_handle, _len_p) fr_radius_ok((_handle)->buffer, (size_t *)(_len_p), \
					    (_handle)->thread->inst->max_attributes, false, NULL)

typedef struct {
	fr_event_list_t		*el;			//!< Event list.

	rlm_radius_t const	*inst;			//!< our instance

	trunk_t			*trunk;			//!< trunk handler
} bio_thread_t;

typedef struct {
	trunk_request_t		*treq;
	rlm_rcode_t		rcode;			//!< from the transport
	bool			is_retry;
} bio_result_t;

typedef struct bio_request_s bio_request_t;

typedef struct {
	struct iovec		out;			//!< Describes buffer to send.
	trunk_request_t	*treq;				//!< Used for signalling.
} bio_coalesced_t;

/** Track the handle, which is tightly correlated with the FD
 *
 */
typedef struct {
	char const		*module_name;		//!< the module that opened the connection

	int			fd;			//!< File descriptor.
	fr_bio_t		*bio;
	fr_bio_fd_info_t const	*fd_info;

	rlm_radius_t const	*inst;			//!< Our module instance.
	bio_thread_t		*thread;
	connection_t		*conn;

	uint8_t			last_id;		//!< Used when replicating to ensure IDs are distributed
							///< evenly.

	uint32_t		max_packet_size;	//!< Our max packet size. may be different from the parent.

	uint8_t			*buffer;		//!< Receive buffer.
	size_t			buflen;			//!< Receive buffer length.

	radius_track_t		*tt;			//!< RADIUS ID tracking structure.

	fr_time_t		mrs_time;		//!< Most recent sent time which had a reply.
	fr_time_t		last_reply;		//!< When we last received a reply.
	fr_time_t		first_sent;		//!< first time we sent a packet since going idle
	fr_time_t		last_sent;		//!< last time we sent a packet.
	fr_time_t		last_idle;		//!< last time we had nothing to do

	fr_event_timer_t const	*zombie_ev;		//!< Zombie timeout.

	bool			status_checking;       	//!< whether we're doing status checks
	bio_request_t		*status_u;		//!< for sending status check packets
	bio_result_t		*status_r;		//!< for faking out status checks as real packets
	request_t		*status_request;
} bio_handle_t;


/** Connect request_t to local tracking structure
 *
 */
struct bio_request_s {
	uint32_t		priority;		//!< copied from request->async->priority
	fr_time_t		recv_time;		//!< copied from request->async->recv_time

	uint32_t		num_replies;		//!< number of reply packets, sent is in retry.count

	bool			require_message_authenticator;		//!< saved from the original packet.
	bool			status_check;		//!< is this packet a status check?
	bool			proxied;		//!< is this request being proxied

	fr_pair_list_t		extra;			//!< VPs for debugging, like Proxy-State.

	uint8_t			code;			//!< Packet code.
	uint8_t			id;			//!< Last ID assigned to this packet.
	uint8_t			*packet;		//!< Packet we write to the network.
	size_t			packet_len;		//!< Length of the packet.
	size_t			partial;		//!< partially sent data

	radius_track_entry_t	*rr;			//!< ID tracking, resend count, etc.
	fr_event_timer_t const	*ev;			//!< timer for retransmissions
	fr_retry_t		retry;			//!< retransmission timers
};


/** Turn a reply code into a module rcode;
 *
 */
static rlm_rcode_t radius_code_to_rcode[FR_RADIUS_CODE_MAX] = {
	[FR_RADIUS_CODE_ACCESS_ACCEPT]		= RLM_MODULE_OK,
	[FR_RADIUS_CODE_ACCESS_CHALLENGE]	= RLM_MODULE_UPDATED,
	[FR_RADIUS_CODE_ACCESS_REJECT]		= RLM_MODULE_REJECT,

	[FR_RADIUS_CODE_ACCOUNTING_RESPONSE]	= RLM_MODULE_OK,

	[FR_RADIUS_CODE_COA_ACK]		= RLM_MODULE_OK,
	[FR_RADIUS_CODE_COA_NAK]		= RLM_MODULE_REJECT,

	[FR_RADIUS_CODE_DISCONNECT_ACK]	= RLM_MODULE_OK,
	[FR_RADIUS_CODE_DISCONNECT_NAK]	= RLM_MODULE_REJECT,

	[FR_RADIUS_CODE_PROTOCOL_ERROR]	= RLM_MODULE_HANDLED,
};

static void		conn_writable_status_check(UNUSED fr_event_list_t *el, UNUSED int fd,
						   UNUSED int flags, void *uctx);

static int 		encode(rlm_radius_t const *inst, request_t *request, bio_request_t *u, uint8_t id);

static decode_fail_t	decode(TALLOC_CTX *ctx, fr_pair_list_t *reply, uint8_t *response_code,
			       bio_handle_t *h, request_t *request, bio_request_t *u,
			       uint8_t const request_authenticator[static RADIUS_AUTH_VECTOR_LENGTH],
			       uint8_t *data, size_t data_len);

static void		protocol_error_reply(bio_request_t *u, bio_result_t *r, bio_handle_t *h);

static unlang_action_t mod_resume(rlm_rcode_t *p_result, module_ctx_t const *mctx, UNUSED request_t *request);
static void mod_signal(module_ctx_t const *mctx, UNUSED request_t *request, fr_signal_t action);
static void mod_write(request_t *request, trunk_request_t *treq, bio_handle_t *h);

#ifndef NDEBUG
/** Log additional information about a tracking entry
 *
 * @param[in] te	Tracking entry we're logging information for.
 * @param[in] log	destination.
 * @param[in] log_type	Type of log message.
 * @param[in] file	the logging request was made in.
 * @param[in] line 	logging request was made on.
 */
static void bio_tracking_entry_log(fr_log_t const *log, fr_log_type_t log_type, char const *file, int line,
				   radius_track_entry_t *te)
{
	request_t			*request;

	if (!te->request) return;	/* Free entry */

	request = talloc_get_type_abort(te->request, request_t);

	fr_log(log, log_type, file, line, "request %s, allocated %s:%d", request->name,
	       request->alloc_file, request->alloc_line);

	trunk_request_state_log(log, log_type, file, line, talloc_get_type_abort(te->uctx, trunk_request_t));
}
#endif

/** Clear out any connection specific resources from a udp request
 *
 */
static void bio_request_reset(bio_request_t *u)
{
	TALLOC_FREE(u->packet);
	fr_pair_list_init(&u->extra);	/* Freed with packet */

	/*
	 *	Can have packet put no u->rr
	 *	if this is part of a pre-trunk status check.
	 */
	if (u->rr) radius_track_entry_release(&u->rr);
}

/** Reset a status_check packet, ready to reuse
 *
 */
static void status_check_reset(bio_handle_t *h, bio_request_t *u)
{
	fr_assert(u->status_check == true);

	h->status_checking = false;
	u->num_replies = 0;	/* Reset */
	u->retry.start = fr_time_wrap(0);

	if (u->ev) (void) fr_event_timer_delete(&u->ev);

	bio_request_reset(u);
}

/*
 *	Status-Server checks.  Manually build the packet, and
 *	all of its associated glue.
 */
static void CC_HINT(nonnull) status_check_alloc(bio_handle_t *h)
{
	bio_request_t		*u;
	request_t		*request;
	rlm_radius_t const	*inst = h->inst;
	map_t			*map = NULL;

	fr_assert(!h->status_u && !h->status_r && !h->status_request);

	u = talloc_zero(h, bio_request_t);
	fr_pair_list_init(&u->extra);

	/*
	 *	Status checks are prioritized over any other packet
	 */
	u->priority = ~(uint32_t) 0;
	u->status_check = true;

	/*
	 *	Allocate outside of the free list.
	 *	There appears to be an issue where
	 *	the thread destructor runs too
	 *	early, and frees the freelist's
	 *	head before the module destructor
	 *      runs.
	 */
	request = request_local_alloc_external(u, NULL);
	request->async = talloc_zero(request, fr_async_t);
	talloc_const_free(request->name);
	request->name = talloc_strdup(request, h->module_name);

	request->packet = fr_packet_alloc(request, false);
	request->reply = fr_packet_alloc(request, false);

	/*
	 *	Create the VPs, and ignore any errors
	 *	creating them.
	 */
	while ((map = map_list_next(&inst->status_check_map, map))) {
		/*
		 *	Skip things which aren't attributes.
		 */
		if (!tmpl_is_attr(map->lhs)) continue;

		/*
		 *	Ignore internal attributes.
		 */
		if (tmpl_attr_tail_da(map->lhs)->flags.internal) continue;

		/*
		 *	Ignore signalling attributes.  They shouldn't exist.
		 */
		if ((tmpl_attr_tail_da(map->lhs) == attr_proxy_state) ||
		    (tmpl_attr_tail_da(map->lhs) == attr_message_authenticator)) continue;

		/*
		 *	Allow passwords only in Access-Request packets.
		 */
		if ((inst->status_check != FR_RADIUS_CODE_ACCESS_REQUEST) &&
		    (tmpl_attr_tail_da(map->lhs) == attr_user_password)) continue;

		(void) map_to_request(request, map, map_to_vp, NULL);
	}

	/*
	 *	Ensure that there's a NAS-Identifier, if one wasn't
	 *	already added.
	 */
	if (!fr_pair_find_by_da(&request->request_pairs, NULL, attr_nas_identifier)) {
		fr_pair_t *vp;

		MEM(pair_append_request(&vp, attr_nas_identifier) >= 0);
		fr_pair_value_strdup(vp, "status check - are you alive?", false);
	}

	/*
	 *	Always add an Event-Timestamp, which will be the time
	 *	at which the first packet is sent.  Or for
	 *	Status-Server, the time of the current packet.
	 */
	if (!fr_pair_find_by_da(&request->request_pairs, NULL, attr_event_timestamp)) {
		MEM(pair_append_request(NULL, attr_event_timestamp) >= 0);
	}

	/*
	 *	Initialize the request IO ctx.  Note that we don't set
	 *	destructors.
	 */
	u->code = inst->status_check;
	request->packet->code = u->code;

	DEBUG3("%s - Status check packet type will be %s", h->module_name, fr_radius_packet_name[u->code]);
	log_request_pair_list(L_DBG_LVL_3, request, NULL, &request->request_pairs, NULL);

	MEM(h->status_r = talloc_zero(request, bio_result_t));
	h->status_u = u;
	h->status_request = request;
}

/** Connection errored
 *
 * We were signalled by the event loop that a fatal error occurred on this connection.
 *
 * @param[in] el	The event list signalling.
 * @param[in] fd	that errored.
 * @param[in] flags	El flags.
 * @param[in] fd_errno	The nature of the error.
 * @param[in] uctx	The trunk connection handle (tconn).
 */
static void conn_error_status_check(UNUSED fr_event_list_t *el, UNUSED int fd, UNUSED int flags, int fd_errno, void *uctx)
{
	connection_t		*conn = talloc_get_type_abort(uctx, connection_t);
	bio_handle_t		*h;

	/*
	 *	Connection must be in the connecting state when this fires
	 */
	fr_assert(conn->state == CONNECTION_STATE_CONNECTING);

	h = talloc_get_type_abort(conn->h, bio_handle_t);

	ERROR("%s - Connection %s failed: %s", h->module_name, h->fd_info->name, fr_syserror(fd_errno));

	connection_signal_reconnect(conn, CONNECTION_FAILED);
}

/** Status check timer when opening the connection for the first time.
 *
 * Setup retries, or fail the connection.
 */
static void conn_status_check_timeout(fr_event_list_t *el, fr_time_t now, void *uctx)
{
	connection_t		*conn = talloc_get_type_abort(uctx, connection_t);
	bio_handle_t		*h;
	bio_request_t		*u;

	/*
	 *	Connection must be in the connecting state when this fires
	 */
	fr_assert(conn->state == CONNECTION_STATE_CONNECTING);

	h = talloc_get_type_abort(conn->h, bio_handle_t);
	u = h->status_u;

	/*
	 *	We're only interested in contiguous, good, replies.
	 */
	u->num_replies = 0;

	switch (fr_retry_next(&u->retry, now)) {
	case FR_RETRY_MRD:
		DEBUG("%s - Reached maximum_retransmit_duration (%pVs > %pVs), failing status checks",
		      h->module_name, fr_box_time_delta(fr_time_sub(now, u->retry.start)),
		      fr_box_time_delta(u->retry.config->mrd));
		goto fail;

	case FR_RETRY_MRC:
		DEBUG("%s - Reached maximum_retransmit_count (%u > %u), failing status checks",
		      h->module_name, u->retry.count, u->retry.config->mrc);
	fail:
		connection_signal_reconnect(conn, CONNECTION_FAILED);
		return;

	case FR_RETRY_CONTINUE:
		if (fr_event_fd_insert(h, NULL, el, h->fd, conn_writable_status_check, NULL,
				       conn_error_status_check, conn) < 0) {
			PERROR("%s - Failed inserting FD event", h->module_name);
			connection_signal_reconnect(conn, CONNECTION_FAILED);
		}
		return;
	}

	fr_assert(0);
}

/** Send the next status check packet
 *
 */
static void conn_status_check_again(fr_event_list_t *el, UNUSED fr_time_t now, void *uctx)
{
	connection_t		*conn = talloc_get_type_abort(uctx, connection_t);
	bio_handle_t		*h = talloc_get_type_abort(conn->h, bio_handle_t);

	if (fr_event_fd_insert(h, NULL, el, h->fd, conn_writable_status_check, NULL, conn_error_status_check, conn) < 0) {
		PERROR("%s - Failed inserting FD event", h->module_name);
		connection_signal_reconnect(conn, CONNECTION_FAILED);
	}
}

/** Read the incoming status-check response.  If it's correct mark the connection as connected
 *
 */
static void conn_readable_status_check(fr_event_list_t *el, UNUSED int fd, UNUSED int flags, void *uctx)
{
	connection_t		*conn = talloc_get_type_abort(uctx, connection_t);
	bio_handle_t		*h = talloc_get_type_abort(conn->h, bio_handle_t);
	trunk_t			*trunk = h->thread->trunk;
	rlm_radius_t const 	*inst = h->inst;
	bio_request_t		*u = h->status_u;
	ssize_t			slen;
	fr_pair_list_t		reply;
	uint8_t			code = 0;

	fr_pair_list_init(&reply);
	slen = read(h->fd, h->buffer, h->buflen);
	if (slen == 0) return;

	if (slen < 0) {
		switch (errno) {
#if defined(EWOULDBLOCK) && (EWOULDBLOCK != EAGAIN)
		case EWOULDBLOCK:
#endif
		case EAGAIN:
		case EINTR:
			return;		/* Wait to be signalled again */

		case ECONNREFUSED:
			ERROR("%s - Failed reading response from socket: there is no server listening on outgoing connection %s",
			      h->module_name, h->fd_info->name);
			break;

		default:
			ERROR("%s - Failed reading response from socket: %s",
			      h->module_name, fr_syserror(errno));
			break;
		}

		connection_signal_reconnect(conn, CONNECTION_FAILED);
		return;
	}

	/*
	 *	Where we just return in this function, we're letting
	 *	the response timer take care of progressing the
	 *	connection attempt.
	 */
	if (slen < RADIUS_HEADER_LENGTH) {
		ERROR("%s - Packet too short, expected at least %zu bytes got %zd bytes",
		      h->module_name, (size_t)RADIUS_HEADER_LENGTH, slen);
		return;
	}

	if (u->id != h->buffer[1]) {
		ERROR("%s - Received response with incorrect or expired ID.  Expected %u, got %u",
		      h->module_name, u->id, h->buffer[1]);
		return;
	}

	if (!check(h, &slen)) return;

	if (decode(h, &reply, &code,
		   h, h->status_request, h->status_u, u->packet + RADIUS_AUTH_VECTOR_OFFSET,
		   h->buffer, slen) != DECODE_FAIL_NONE) return;

	fr_pair_list_free(&reply);	/* FIXME - Do something with these... */

	/*
	 *	Process the error, and count this as a success.
	 *	This is usually used for dynamic configuration
	 *	on startup.
	 */
	if (code == FR_RADIUS_CODE_PROTOCOL_ERROR) protocol_error_reply(u, NULL, h);

	/*
	 *	Last trunk event was a failure, be more careful about
	 *	bringing up the connection (require multiple responses).
	 */
	if ((fr_time_gt(trunk->last_failed, fr_time_wrap(0)) && (fr_time_gt(trunk->last_failed, trunk->last_connected))) &&
	    (u->num_replies < inst->num_answers_to_alive)) {
		/*
		 *	Leave the timer in place.  This timer is BOTH when we
		 *	give up on the current status check, AND when we send
		 *	the next status check.
		 */
		DEBUG("%s - Received %u / %u replies for status check, on connection - %s",
		      h->module_name, u->num_replies, inst->num_answers_to_alive, h->fd_info->name);
		DEBUG("%s - Next status check packet will be in %pVs",
		      h->module_name, fr_box_time_delta(fr_time_sub(u->retry.next, fr_time())));

		/*
		 *	Set the timer for the next retransmit.
		 */
		if (fr_event_timer_at(h, el, &u->ev, u->retry.next, conn_status_check_again, conn) < 0) {
			connection_signal_reconnect(conn, CONNECTION_FAILED);
		}
		return;
	}

	/*
	 *	It's alive!
	 */
	status_check_reset(h, u);

	DEBUG("%s - Connection open - %s", h->module_name, h->fd_info->name);

	connection_signal_connected(conn);
}

/** Send our status-check packet as soon as the connection becomes writable
 *
 */
static void conn_writable_status_check(fr_event_list_t *el, UNUSED int fd, UNUSED int flags, void *uctx)
{
	connection_t		*conn = talloc_get_type_abort(uctx, connection_t);
	bio_handle_t		*h = talloc_get_type_abort(conn->h, bio_handle_t);
	bio_request_t		*u = h->status_u;
	ssize_t			slen;

	if (fr_time_eq(u->retry.start, fr_time_wrap(0))) {
		u->id = fr_rand() & 0xff;	/* We don't care what the value is here */
		h->status_checking = true;	/* Ensure this is valid */
		fr_retry_init(&u->retry, fr_time(), &h->inst->retry[u->code]);

	/*
	 *	Status checks can never be retransmitted
	 *	So increment the ID here.
	 */
	} else {
		bio_request_reset(u);
		u->id++;
	}

	DEBUG("%s - Sending %s ID %d over connection %s",
	      h->module_name, fr_radius_packet_name[u->code], u->id, h->fd_info->name);

	if (encode(h->inst, h->status_request, u, u->id) < 0) {
	fail:
		connection_signal_reconnect(conn, CONNECTION_FAILED);
		return;
	}
	DEBUG3("Encoded packet");
	HEXDUMP3(u->packet, u->packet_len, NULL);

	slen = write(h->fd, u->packet, u->packet_len);
	if (slen < 0) {
		ERROR("%s - Failed sending %s ID %d length %zu over connection %s: %s",
		      h->module_name, fr_radius_packet_name[u->code], u->id, u->packet_len, h->fd_info->name, fr_syserror(errno));


		goto fail;
	}
	fr_assert((size_t)slen == u->packet_len);

	/*
	 *	Switch to waiting on read and insert the event
	 *	for the response timeout.
	 */
	if (fr_event_fd_insert(h, NULL, conn->el, h->fd, conn_readable_status_check, NULL, conn_error_status_check, conn) < 0) {
		PERROR("%s - Failed inserting FD event", h->module_name);
		goto fail;
	}

	DEBUG("%s - %s request.  Expecting response within %pVs",
	      h->module_name, (u->retry.count == 1) ? "Originated" : "Retransmitted",
	      fr_box_time_delta(u->retry.rt));

	if (fr_event_timer_at(u, el, &u->ev, u->retry.next, conn_status_check_timeout, conn) < 0) {
		PERROR("%s - Failed inserting timer event", h->module_name);
		goto fail;
	}
}

/** Free a connection handle, closing associated resources
 *
 */
static int _bio_handle_free(bio_handle_t *h)
{
	fr_assert(h->fd >= 0);

	if (h->status_u) fr_event_timer_delete(&h->status_u->ev);

	fr_event_fd_delete(h->thread->el, h->fd, FR_EVENT_FILTER_IO);

	if (shutdown(h->fd, SHUT_RDWR) < 0) {
		DEBUG3("%s - Failed shutting down connection %s: %s",
		       h->module_name, h->fd_info->name, fr_syserror(errno));
	}

	if (close(h->fd) < 0) {
		DEBUG3("%s - Failed closing connection %s: %s",
		       h->module_name, h->fd_info->name, fr_syserror(errno));
	}

	h->fd = -1;

	DEBUG("%s - Connection closed - %s", h->module_name, h->fd_info->name);

	return 0;
}

static void bio_connected(fr_bio_t *bio)
{
	bio_handle_t		*h = bio->uctx;

	DEBUG("%s - Connection open - %s", h->module_name, h->fd_info->name);

	connection_signal_connected(h->conn);
}

static void bio_error(fr_bio_t *bio)
{
	bio_handle_t		*h = bio->uctx;

	DEBUG("%s - Connection failed - %s - %s", h->module_name, h->fd_info->name,
	      fr_syserror(h->fd_info->connect_errno));

	connection_signal_reconnect(h->conn, CONNECTION_FAILED);
}

/** Initialise a new outbound connection
 *
 * @param[out] h_out	Where to write the new file descriptor.
 * @param[in] conn	to initialise.
 * @param[in] uctx	A #bio_thread_t
 */
CC_NO_UBSAN(function) /* UBSAN: false positive - public vs private connection_t trips --fsanitize=function*/
static connection_state_t conn_init(void **h_out, connection_t *conn, void *uctx)
{
	int			fd;
	bio_handle_t		*h;
	bio_thread_t		*thread = talloc_get_type_abort(uctx, bio_thread_t);

	MEM(h = talloc_zero(conn, bio_handle_t));
	h->thread = thread;
	h->conn = conn;
	h->inst = thread->inst;
	h->module_name = h->inst->name;
	h->max_packet_size = h->inst->max_packet_size;
	h->last_idle = fr_time();

	MEM(h->buffer = talloc_array(h, uint8_t, h->max_packet_size));
	h->buflen = h->max_packet_size;

	MEM(h->tt = radius_track_alloc(h));

	h->bio = fr_bio_fd_alloc(h, &h->inst->fd_config, 0);
	if (!h->bio) {
		PERROR("%s - ", h->module_name);
	fail:
		talloc_free(h);
		return CONNECTION_STATE_FAILED;
	}

	h->bio->uctx = h;
	h->fd_info = fr_bio_fd_info(h->bio);
	fd = h->fd_info->socket.fd;

	fr_assert(fd >= 0);

	talloc_set_destructor(h, _bio_handle_free);

	h->fd = fd;

	/*
	 *	If the socket isn't connected, then do that first.
	 */
	if (h->fd_info->state != FR_BIO_FD_STATE_OPEN) {
		int rcode;

		fr_assert(h->fd_info->state == FR_BIO_FD_STATE_CONNECTING);

		/*
		 *	@todo - call connect_full() with callbacks, timeouts, etc.
		 */
		rcode = fr_bio_fd_connect_full(h->bio, conn->el, bio_connected, bio_error, NULL, NULL);
		if (rcode < 0) goto fail;

		if (rcode == 0) return CONNECTION_STATE_CONNECTING;

		fr_assert(rcode == 1);
		return CONNECTION_STATE_CONNECTED;

		/*
		 *	If we're doing status checks, then we want at least
		 *	one positive response before signalling that the
		 *	connection is open.
		 *
		 *	To do this we install special I/O handlers that
		 *	only signal the connection as open once we get a
		 *	status-check response.
		 */
	} if (h->inst->status_check) {
		status_check_alloc(h);

		/*
		 *	Start status checking.
		 *
		 *	If we've had no recent failures we need exactly
		 *	one response to bring the connection online,
		 *	otherwise we need inst->num_answers_to_alive
		 */
		if (fr_event_fd_insert(h, NULL, conn->el, h->fd, NULL,
				       conn_writable_status_check, conn_error_status_check, conn) < 0) goto fail;

		/*
		 *	If we're not doing status-checks, signal the connection
		 *	as open as soon as it becomes writable.
		 */
	} else {
		connection_signal_on_fd(conn, fd);
	}

	*h_out = h;

	return CONNECTION_STATE_CONNECTING;
}

/** Shutdown/close a file descriptor
 *
 */
static void conn_close(UNUSED fr_event_list_t *el, void *handle, UNUSED void *uctx)
{
	bio_handle_t *h = talloc_get_type_abort(handle, bio_handle_t);

	/*
	 *	There's tracking entries still allocated
	 *	this is bad, they should have all been
	 *	released.
	 */
	if (h->tt && (h->tt->num_requests != 0)) {
#ifndef NDEBUG
		radius_track_state_log(&default_log, L_ERR, __FILE__, __LINE__, h->tt, bio_tracking_entry_log);
#endif
		fr_assert_fail("%u tracking entries still allocated at conn close", h->tt->num_requests);
	}

	DEBUG4("Freeing rlm_radius_udp handle %p", handle);

	talloc_free(h);
}

/** Connection failed
 *
 * @param[in] handle   	of connection that failed.
 * @param[in] state	the connection was in when it failed.
 * @param[in] uctx	UNUSED.
 */
static connection_state_t conn_failed(void *handle, connection_state_t state, UNUSED void *uctx)
{
	switch (state) {
	/*
	 *	If the connection was connected when it failed,
	 *	we need to handle any outstanding packets and
	 *	timer events before reconnecting.
	 */
	case CONNECTION_STATE_CONNECTED:
	{
		bio_handle_t	*h = talloc_get_type_abort(handle, bio_handle_t); /* h only available if connected */

		/*
		 *	Reset the Status-Server checks.
		 */
		if (h->status_u && h->status_u->ev) (void) fr_event_timer_delete(&h->status_u->ev);
	}
		break;

	default:
		break;
	}

	return CONNECTION_STATE_INIT;
}

CC_NO_UBSAN(function) /* UBSAN: false positive - public vs private connection_t trips --fsanitize=function*/
static connection_t *thread_conn_alloc(trunk_connection_t *tconn, fr_event_list_t *el,
					  connection_conf_t const *conf,
					  char const *log_prefix, void *uctx)
{
	connection_t		*conn;
	bio_thread_t		*thread = talloc_get_type_abort(uctx, bio_thread_t);

	conn = connection_alloc(tconn, el,
				   &(connection_funcs_t){
					.init = conn_init,
					.close = conn_close,
					.failed = conn_failed
				   },
				   conf,
				   log_prefix,
				   thread);
	if (!conn) {
		PERROR("%s - Failed allocating state handler for new connection", thread->inst->name);
		return NULL;
	}

	return conn;
}

/** Read and discard data
 *
 */
static void conn_discard(UNUSED fr_event_list_t *el, int fd, UNUSED int flags, void *uctx)
{
	trunk_connection_t	*tconn = talloc_get_type_abort(uctx, trunk_connection_t);
	bio_handle_t		*h = talloc_get_type_abort(tconn->conn->h, bio_handle_t);
	uint8_t			buffer[4096];
	ssize_t			slen;

	while ((slen = read(fd, buffer, sizeof(buffer))) > 0);

	if (slen < 0) {
		switch (errno) {
		case EBADF:
		case ECONNRESET:
		case ENOTCONN:
		case ETIMEDOUT:
			ERROR("%s - Failed draining socket: %s", h->module_name, fr_syserror(errno));
			trunk_connection_signal_reconnect(tconn, CONNECTION_FAILED);
			break;

		default:
			break;
		}
	}
}

/** Connection errored
 *
 * We were signalled by the event loop that a fatal error occurred on this connection.
 *
 * @param[in] el	The event list signalling.
 * @param[in] fd	that errored.
 * @param[in] flags	El flags.
 * @param[in] fd_errno	The nature of the error.
 * @param[in] uctx	The trunk connection handle (tconn).
 */
static void conn_error(UNUSED fr_event_list_t *el, UNUSED int fd, UNUSED int flags, int fd_errno, void *uctx)
{
	trunk_connection_t	*tconn = talloc_get_type_abort(uctx, trunk_connection_t);
	connection_t		*conn = tconn->conn;
	bio_handle_t		*h = talloc_get_type_abort(conn->h, bio_handle_t);

	ERROR("%s - Connection %s failed: %s", h->module_name, h->fd_info->name, fr_syserror(fd_errno));

	connection_signal_reconnect(conn, CONNECTION_FAILED);
}

CC_NO_UBSAN(function) /* UBSAN: false positive - public vs private connection_t trips --fsanitize=function*/
static void thread_conn_notify(trunk_connection_t *tconn, connection_t *conn,
			       fr_event_list_t *el,
			       trunk_connection_event_t notify_on, UNUSED void *uctx)
{
	bio_handle_t		*h = talloc_get_type_abort(conn->h, bio_handle_t);
	fr_event_fd_cb_t	read_fn = NULL;
	fr_event_fd_cb_t	write_fn = NULL;

	switch (notify_on) {
		/*
		 *	We may have sent multiple requests to the
		 *	other end, so it might be sending us multiple
		 *	replies.  We want to drain the socket, instead
		 *	of letting the packets sit in the UDP receive
		 *	queue.
		 */
	case TRUNK_CONN_EVENT_NONE:
		read_fn = conn_discard;
		break;

	case TRUNK_CONN_EVENT_READ:
		read_fn = trunk_connection_callback_readable;
		break;

	case TRUNK_CONN_EVENT_WRITE:
		write_fn = trunk_connection_callback_writable;
		break;

	case TRUNK_CONN_EVENT_BOTH:
		read_fn = trunk_connection_callback_readable;
		write_fn = trunk_connection_callback_writable;
		break;

	}

	/*
	 *	Over-ride read for replication.
	 */
	if (h->inst->mode == RLM_RADIUS_MODE_REPLICATE) {
		read_fn = conn_discard;

		if (fr_bio_fd_write_only(h->bio) < 0) {
			PERROR("%s - Failed setting socket to write-only", h->module_name);
			trunk_connection_signal_reconnect(tconn, CONNECTION_FAILED);
			return;
		}
	}

	if (fr_event_fd_insert(h, NULL, el, h->fd,
			       read_fn,
			       write_fn,
			       conn_error,
			       tconn) < 0) {
		PERROR("%s - Failed inserting FD event", h->module_name);

		/*
		 *	May free the connection!
		 */
		trunk_connection_signal_reconnect(tconn, CONNECTION_FAILED);
	}
}

/*
 *  Return negative numbers to put 'a' at the top of the heap.
 *  Return positive numbers to put 'b' at the top of the heap.
 *
 *  We want the value with the lowest timestamp to be prioritized at
 *  the top of the heap.
 */
static int8_t request_prioritise(void const *one, void const *two)
{
	bio_request_t const *a = one;
	bio_request_t const *b = two;
	int8_t ret;

	// @todo - prioritize packets if there's a state?

	/*
	 *	Prioritise status check packets
	 */
	ret = (b->status_check - a->status_check);
	if (ret != 0) return ret;

	/*
	 *	Larger priority is more important.
	 */
	ret = CMP(a->priority, b->priority);
	if (ret != 0) return ret;

	/*
	 *	Smaller timestamp (i.e. earlier) is more important.
	 */
	return CMP_PREFER_SMALLER(fr_time_unwrap(a->recv_time), fr_time_unwrap(b->recv_time));
}

/** Decode response packet data, extracting relevant information and validating the packet
 *
 * @param[in] ctx			to allocate pairs in.
 * @param[out] reply			Pointer to head of pair list to add reply attributes to.
 * @param[out] response_code		The type of response packet.
 * @param[in] h				connection handle.
 * @param[in] request			the request.
 * @param[in] u				UDP request.
 * @param[in] request_authenticator	from the original request.
 * @param[in] data			to decode.
 * @param[in] data_len			Length of input data.
 * @return
 *	- DECODE_FAIL_NONE on success.
 *	- DECODE_FAIL_* on failure.
 */
static decode_fail_t decode(TALLOC_CTX *ctx, fr_pair_list_t *reply, uint8_t *response_code,
			    bio_handle_t *h, request_t *request, bio_request_t *u,
			    uint8_t const request_authenticator[static RADIUS_AUTH_VECTOR_LENGTH],
			    uint8_t *data, size_t data_len)
{
	rlm_radius_t const	*inst = talloc_get_type_abort_const(h->thread->inst, rlm_radius_t);
	uint8_t			code;
	fr_radius_decode_ctx_t	decode_ctx;

	*response_code = 0;	/* Initialise to keep the rest of the code happy */

	RHEXDUMP3(data, data_len, "Read packet");

	decode_ctx = (fr_radius_decode_ctx_t) {
		.common = &inst->common_ctx,
		.request_code = u->code,
		.request_authenticator = request_authenticator,
		.tmp_ctx = talloc(ctx, uint8_t),
		.end = data + data_len,
		.verify = true,
		.require_message_authenticator = ((*(inst->received_message_authenticator) & inst->require_message_authenticator) |
						  (inst->require_message_authenticator & FR_RADIUS_REQUIRE_MA_YES)) > 0
	};

	if (fr_radius_decode(ctx, reply, data, data_len, &decode_ctx) < 0) {
		talloc_free(decode_ctx.tmp_ctx);
		RPEDEBUG("Failed reading packet");
		return DECODE_FAIL_UNKNOWN;
	}
	talloc_free(decode_ctx.tmp_ctx);

	code = data[0];

	RDEBUG("Received %s ID %d length %zu reply packet on connection %s",
	       fr_radius_packet_name[code], data[1], data_len, h->fd_info->name);
	log_request_pair_list(L_DBG_LVL_2, request, NULL, reply, NULL);

	/*
	 *	This code is for BlastRADIUS mitigation.
	 *
	 *	The scenario where this applies is where we send Message-Authenticator
	 *	but the home server doesn't support it or require it, in which case
	 *	the response can be manipulated by an attacker.
	 */
	if (u->code == FR_RADIUS_CODE_ACCESS_REQUEST) {
		if ((inst->require_message_authenticator == FR_RADIUS_REQUIRE_MA_AUTO) &&
		    !*(inst->received_message_authenticator) &&
		    fr_pair_find_by_da(&request->request_pairs, NULL, attr_message_authenticator) &&
		    !fr_pair_find_by_da(&request->request_pairs, NULL, attr_eap_message)) {
			RINFO("Packet contained a valid Message-Authenticator.  Setting \"require_message_authenticator = yes\"");
			*(inst->received_message_authenticator) = true;
		}
	}

	*response_code = code;

	/*
	 *	Record the fact we've seen a response
	 */
	u->num_replies++;

	/*
	 *	Fixup retry times
	 */
	if (fr_time_gt(u->retry.start, h->mrs_time)) h->mrs_time = u->retry.start;

	return DECODE_FAIL_NONE;
}

static int encode(rlm_radius_t const *inst, request_t *request, bio_request_t *u, uint8_t id)
{
	ssize_t			packet_len;
	fr_radius_encode_ctx_t	encode_ctx;

	fr_assert(inst->allowed[u->code]);
	fr_assert(!u->packet);

	/*
	 *	This is essentially free, as this memory was
	 *	pre-allocated as part of the treq.
	 */
	u->packet_len = inst->max_packet_size;
	MEM(u->packet = talloc_array(u, uint8_t, u->packet_len));

	/*
	 *	We should have at minimum 64-byte packets, so don't
	 *	bother doing run-time checks here.
	 */
	fr_assert(u->packet_len >= (size_t) RADIUS_HEADER_LENGTH);

	encode_ctx = (fr_radius_encode_ctx_t) {
		.common = &inst->common_ctx,
		.rand_ctx = (fr_fast_rand_t) {
			.a = fr_rand(),
			.b = fr_rand(),
		},
		.code = u->code,
		.id = id,
		.add_proxy_state = u->proxied,
	};

	/*
	 *	If we're sending a status check packet, update any
	 *	necessary timestamps.  Also, don't add Proxy-State, as
	 *	we're originating the packet.
	 */
	if (u->status_check) {
		fr_pair_t *vp;

		vp = fr_pair_find_by_da(&request->request_pairs, NULL, attr_event_timestamp);
		if (vp) vp->vp_date = fr_time_to_unix_time(u->retry.updated);

		encode_ctx.add_proxy_state = false;
	}

	/*
	 *	Encode it, leaving room for Proxy-State if necessary.
	 */
	packet_len = fr_radius_encode(&FR_DBUFF_TMP(u->packet, u->packet_len),
				      &request->request_pairs, &encode_ctx);
	if (fr_pair_encode_is_error(packet_len)) {
		RPERROR("Failed encoding packet");

	error:
		TALLOC_FREE(u->packet);
		return -1;
	}

	if (packet_len < 0) {
		size_t have;
		size_t need;

		have = u->packet_len;
		need = have - packet_len;

		if (need > RADIUS_MAX_PACKET_SIZE) {
			RERROR("Failed encoding packet.  Have %zu bytes of buffer, need %zu bytes",
			       have, need);
		} else {
			RERROR("Failed encoding packet.  Have %zu bytes of buffer, need %zu bytes.  "
			       "Increase 'max_packet_size'", have, need);
		}

		goto error;
	}
	/*
	 *	The encoded packet should NOT over-run the input buffer.
	 */
	fr_assert((size_t) packet_len <= u->packet_len);

	/*
	 *	Add Proxy-State to the tail end of the packet.
	 *
	 *	We need to add it here, and NOT in
	 *	request->request_pairs, because multiple modules
	 *	may be sending the packets at the same time.
	 */
	if (encode_ctx.add_proxy_state) {
		fr_pair_t	*vp;

		MEM(vp = fr_pair_afrom_da(u->packet, attr_proxy_state));
		fr_pair_value_memdup(vp, (uint8_t const *) &inst->common_ctx.proxy_state, sizeof(inst->common_ctx.proxy_state), false);
		fr_pair_append(&u->extra, vp);
	}

	/*
	 *	Update our version of the packet length.
	 */
	u->packet_len = packet_len;

	/*
	 *	Now that we're done mangling the packet, sign it.
	 */
	if (fr_radius_sign(u->packet, NULL, (uint8_t const *) inst->secret,
			   talloc_array_length(inst->secret) - 1) < 0) {
		RERROR("Failed signing packet");
		goto error;
	}

	return 0;
}


/** Revive a connection after "revive_interval"
 *
 */
static void revive_timeout(UNUSED fr_event_list_t *el, UNUSED fr_time_t now, void *uctx)
{
	trunk_connection_t	*tconn = talloc_get_type_abort(uctx, trunk_connection_t);
	bio_handle_t	 	*h = talloc_get_type_abort(tconn->conn->h, bio_handle_t);

	INFO("%s - Reviving connection %s", h->module_name, h->fd_info->name);
	trunk_connection_signal_reconnect(tconn, CONNECTION_FAILED);
}

/** Mark a connection dead after "zombie_interval"
 *
 */
static void zombie_timeout(fr_event_list_t *el, fr_time_t now, void *uctx)
{
	trunk_connection_t	*tconn = talloc_get_type_abort(uctx, trunk_connection_t);
	bio_handle_t	 	*h = talloc_get_type_abort(tconn->conn->h, bio_handle_t);

	INFO("%s - No replies during 'zombie_period', marking connection %s as dead", h->module_name, h->fd_info->name);

	/*
	 *	Don't use this connection, and re-queue all of its
	 *	requests onto other connections.
	 */
	(void) trunk_connection_requests_requeue(tconn, TRUNK_REQUEST_STATE_ALL, 0, false);

	/*
	 *	We do have status checks.  Try to reconnect the
	 *	connection immediately.  If the status checks pass,
	 *	then the connection will be marked "alive"
	 */
	if (h->inst->status_check) {
		trunk_connection_signal_reconnect(tconn, CONNECTION_FAILED);
		return;
	}

	/*
	 *	Revive the connection after a time.
	 */
	if (fr_event_timer_at(h, el, &h->zombie_ev,
			      fr_time_add(now, h->inst->revive_interval), revive_timeout, tconn) < 0) {
		ERROR("Failed inserting revive timeout for connection");
		trunk_connection_signal_reconnect(tconn, CONNECTION_FAILED);
	}
}


/** See if the connection is zombied.
 *
 *	We check for zombie when major events happen:
 *
 *	1) request hits its final timeout
 *	2) request timer hits, and it needs to be retransmitted
 *	3) a DUP packet comes in, and the request needs to be retransmitted
 *	4) we're sending a packet.
 *
 *  There MIGHT not be retries configured, so we MUST check for zombie
 *  when any new packet comes in.  Similarly, there MIGHT not be new
 *  packets, but retries are configured, so we have to check there,
 *  too.
 *
 *  Also, the socket might not be writable for a while.  There MIGHT
 *  be a long time between getting the timer / DUP signal, and the
 *  request finally being written to the socket.  So we need to check
 *  for zombie at BOTH the timeout and the mux / write function.
 *
 * @return
 *	- true if the connection is zombie.
 *	- false if the connection is not zombie.
 */
static bool check_for_zombie(fr_event_list_t *el, trunk_connection_t *tconn, fr_time_t now, fr_time_t last_sent)
{
	bio_handle_t	*h = talloc_get_type_abort(tconn->conn->h, bio_handle_t);

	/*
	 *	We're replicating, and don't care about the health of
	 *	the home server, and this function should not be called.
	 */
	fr_assert(h->inst->mode != RLM_RADIUS_MODE_REPLICATE);

	/*
	 *	If we're status checking OR already zombie, don't go to zombie
	 */
	if (h->status_checking || h->zombie_ev) return true;

	if (fr_time_eq(now, fr_time_wrap(0))) now = fr_time();

	/*
	 *	We received a reply since this packet was sent, the connection isn't zombie.
	 */
	if (fr_time_gteq(h->last_reply, last_sent)) return false;

	/*
	 *	If we've seen ANY response in the allowed window, then the connection is still alive.
	 */
	if ((h->inst->mode == RLM_RADIUS_MODE_PROXY) && fr_time_gt(last_sent, fr_time_wrap(0)) &&
	    (fr_time_lt(fr_time_add(last_sent, h->inst->response_window), now))) return false;

	/*
	 *	Stop using it for new requests.
	 */
	WARN("%s - Entering Zombie state - connection %s", h->module_name, h->fd_info->name);
	trunk_connection_signal_inactive(tconn);

	if (h->inst->status_check) {
		h->status_checking = true;

		/*
		 *	Queue up the status check packet.  It will be sent
		 *	when the connection is writable.
		 */
		h->status_u->retry.start = fr_time_wrap(0);
		h->status_r->treq = NULL;

		if (trunk_request_enqueue_on_conn(&h->status_r->treq, tconn, h->status_request,
						     h->status_u, h->status_r, true) != TRUNK_ENQUEUE_OK) {
			trunk_connection_signal_reconnect(tconn, CONNECTION_FAILED);
		}
	} else {
		if (fr_event_timer_at(h, el, &h->zombie_ev, fr_time_add(now, h->inst->zombie_period),
				      zombie_timeout, tconn) < 0) {
			ERROR("Failed inserting zombie timeout for connection");
			trunk_connection_signal_reconnect(tconn, CONNECTION_FAILED);
		}
	}

	return true;
}


/** Handle retries.
 *
 */
static void mod_retry(module_ctx_t const *mctx, request_t *request, fr_retry_t const *retry)
{
	bio_result_t		*r = talloc_get_type_abort(mctx->rctx, bio_result_t);
	rlm_radius_t const     	*inst = talloc_get_type_abort(mctx->mi->data, rlm_radius_t);
	fr_time_t		now = retry->updated;

	trunk_request_t		*treq = talloc_get_type_abort(r->treq, trunk_request_t);
	trunk_connection_t	*tconn = treq->tconn;

	bio_request_t		*u = talloc_get_type_abort(treq->preq, bio_request_t);

	bio_handle_t		*h;

	fr_assert(request == treq->request);
	fr_assert(treq->preq);						/* Must still have a protocol request */

	switch (retry->state) {
	case FR_RETRY_CONTINUE:
		u->retry = *retry;

		switch (treq->state) {
		case TRUNK_REQUEST_STATE_INIT:
		case TRUNK_REQUEST_STATE_UNASSIGNED:
			fr_assert(0);
			break;

		case TRUNK_REQUEST_STATE_BACKLOG:
			RDEBUG("Request is still in the backlog queue to be sent - suppressing retransmission");
			return;

		case TRUNK_REQUEST_STATE_PENDING:
			RDEBUG("Request is still in the pending queue to be sent - suppressing retransmission");
			return;

		case TRUNK_REQUEST_STATE_PARTIAL:
			RDEBUG("Request was partially written, as IO is blocked - suppressing retransmission");
			return;

		case TRUNK_REQUEST_STATE_SENT:
			fr_assert(tconn);

			h = talloc_get_type_abort(tconn->conn->h, bio_handle_t);

			if (h->fd_info->write_blocked) {
				RDEBUG("IO is blocked - suppressing retransmission");
				return;
			}

			r->is_retry = true;
			mod_write(request, treq, h);
			return;

		case TRUNK_REQUEST_STATE_REAPABLE:
		case TRUNK_REQUEST_STATE_COMPLETE:
		case TRUNK_REQUEST_STATE_FAILED:
		case TRUNK_REQUEST_STATE_CANCEL:
		case TRUNK_REQUEST_STATE_CANCEL_SENT:
		case TRUNK_REQUEST_STATE_CANCEL_PARTIAL:
		case TRUNK_REQUEST_STATE_CANCEL_COMPLETE:
			fr_assert(0);
			break;
		}
		break;

	case FR_RETRY_MRD:
		REDEBUG("Reached maximum_retransmit_duration (%pVs > %pVs), failing request",
			fr_box_time_delta(fr_time_sub(now, retry->start)), fr_box_time_delta(retry->config->mrd));
		break;

	case FR_RETRY_MRC:
		REDEBUG("Reached maximum_retransmit_count (%u > %u), failing request",
		        retry->count, retry->config->mrc);
		break;
	}

	r->rcode = RLM_MODULE_FAIL;
	trunk_request_signal_fail(treq);

	/*
	 *	We don't do zombie stuff!
	 */
	if (!tconn || (inst->mode == RLM_RADIUS_MODE_REPLICATE)) return;

	check_for_zombie(unlang_interpret_event_list(request), tconn, now, retry->start);
}

CC_NO_UBSAN(function) /* UBSAN: false positive - public vs private connection_t trips --fsanitize=function*/
static void request_mux(UNUSED fr_event_list_t *el,
			trunk_connection_t *tconn, connection_t *conn, UNUSED void *uctx)
{
	bio_handle_t		*h = talloc_get_type_abort(conn->h, bio_handle_t);
	trunk_request_t		*treq;

	if (unlikely(trunk_connection_pop_request(&treq, tconn) < 0)) return;

	/*
	 *	No more requests to send
	 */
	if (!treq) return;

	mod_write(treq->request, treq, h);
}

static void mod_write(request_t *request, trunk_request_t *treq, bio_handle_t *h)
{
	rlm_radius_t const	*inst = h->inst;
	bio_request_t		*u;
	char const		*action;
	uint8_t const		*packet;
	size_t			packet_len;
	ssize_t			slen;

	fr_assert((treq->state == TRUNK_REQUEST_STATE_PENDING) ||
		  (treq->state == TRUNK_REQUEST_STATE_PARTIAL));

	u = talloc_get_type_abort(treq->preq, bio_request_t);

	fr_assert(!u->status_check);

	/*
	 *	If it's a partial packet, then write the partial bit.
	 */
	if (u->partial) {
		fr_assert(u->partial < u->packet_len);
		packet = u->packet + u->partial;
		packet_len = u->packet_len - u->partial;
		goto do_write;
	}

	/*
	 *	No previous packet, OR can't retransmit the
	 *	existing one.  Oh well.
	 *
	 *	Note that if we can't retransmit the previous
	 *	packet, then u->rr MUST already have been
	 *	deleted in the request_cancel() function
	 *	or request_release_conn() function when
	 *	the REQUEUE signal was received.
	 */
	if (!u->packet) {
		fr_assert(!u->rr);

		if (unlikely(radius_track_entry_reserve(&u->rr, treq, h->tt, request, u->code, treq) < 0)) {
#ifndef NDEBUG
			radius_track_state_log(&default_log, L_ERR, __FILE__, __LINE__,
					       h->tt, bio_tracking_entry_log);
#endif
			fr_assert_fail("Tracking entry allocation failed: %s", fr_strerror());
			trunk_request_signal_fail(treq);
			return;
		}
		u->id = u->rr->id;

		RDEBUG("Sending %s ID %d length %zu over connection %s",
		       fr_radius_packet_name[u->code], u->id, u->packet_len, h->fd_info->name);

		if (encode(h->inst, request, u, u->id) < 0) {
			/*
			 *	Need to do this because request_conn_release
			 *	may not be called.
			 */
			bio_request_reset(u);
			if (u->ev) (void) fr_event_timer_delete(&u->ev);
			trunk_request_signal_fail(treq);
			return;
		}
		RHEXDUMP3(u->packet, u->packet_len, "Encoded packet");

		/*
		 *	Remember the authentication vector, which now has the
		 *	packet signature.
		 */
		(void) radius_track_entry_update(u->rr, u->packet + RADIUS_AUTH_VECTOR_OFFSET);
	} else {
		RDEBUG("Retransmitting %s ID %d length %zu over connection %s",
		       fr_radius_packet_name[u->code], u->id, u->packet_len, h->fd_info->name);
	}

	log_request_pair_list(L_DBG_LVL_2, request, NULL, &request->request_pairs, NULL);
	if (!fr_pair_list_empty(&u->extra)) log_request_pair_list(L_DBG_LVL_2, request, NULL, &u->extra, NULL);

	packet = u->packet;
	packet_len = u->packet_len;

do_write:
	slen = fr_bio_write(h->bio, NULL, packet, packet_len);
	if (slen < 0) {
		/*
		 *	@todo - check slen for fr_bio_error(FOO)
		 */

		/*
		 *	Temporary conditions
		 */
		switch (errno) {
			/*
			 *	The BIO code should catch EAGAIN, EWOULDBLOCK, EINTR,
			 *	and return "0 bytes written".
			 */
#if defined(EWOULDBLOCK) && (EWOULDBLOCK != EAGAIN)
		case EWOULDBLOCK:	/* No outbound packet buffers, maybe? */
#endif
		case EAGAIN:		/* No outbound packet buffers, maybe? */
		case EINTR:		/* Interrupted by signal */

		case ENOBUFS:		/* No outbound packet buffers, maybe? */
		case ENOMEM:		/* malloc failure in kernel? */
			RWARN("%s - Failed sending data over connection %s: %s",
			      h->module_name, h->fd_info->name, fr_syserror(errno));
			trunk_request_signal_fail(treq);
			break;

		/*
		 *	Fatal, request specific conditions
		 */
		case EMSGSIZE:		/* Packet size exceeds max size allowed on socket */
			ERROR("%s - Failed sending data over connection %s: %s",
			      h->module_name, h->fd_info->name, fr_syserror(errno));
			trunk_request_signal_fail(treq);
			break;

		/*
		 *	Will re-queue any 'sent' requests, so we don't
		 *	have to do any cleanup.
		 */
		default:
			ERROR("%s - Failed sending data over connection %s: %s",
			      h->module_name, h->fd_info->name, fr_syserror(errno));
			trunk_connection_signal_reconnect(treq->tconn, CONNECTION_FAILED);
			break;
		}

		return;
	}

	/*
	 *	No data to send, ignore the write for partials, but otherwise requeue it.
	 */
	if (slen == 0) {
		if (u->partial) return;

		RWARN("%s - Failed sending data over connection %s: sent zero bytes",
		      h->module_name, h->fd_info->name);
		trunk_request_requeue(treq);
		return;
	}

	packet_len += slen;
	if (packet_len < u->packet_len) {
		u->partial = packet_len;
		trunk_request_signal_partial(treq);
		return;
	}

	/*
	 *	For retransmissions.
	 */
	u->partial = 0;

	/*
	 *	Don't print anything extra for replication.
	 */
	if (inst->mode == RLM_RADIUS_MODE_REPLICATE) {
		bio_result_t *r = talloc_get_type_abort(treq->rctx, bio_result_t);

		r->rcode = RLM_MODULE_OK;
		trunk_request_signal_complete(treq);
		return;
	}

	/*
	 *	On first packet, signal it as sent, and update stats.
	 *
	 *	Later packets are just retransmissions to the BIO, and don't need to involve
	 *	the trunk code.
	 */
	if (u->retry.count == 1) {
		h->last_sent = u->retry.start;
		if (fr_time_lteq(h->first_sent, h->last_idle)) h->first_sent = h->last_sent;

		trunk_request_signal_sent(treq);

		action = u->proxied ? "Proxied" : "Originated";

	} else {
		/*
		 *	We don't signal the trunk that it's been sent, it was already senty
		 */
		action = "Retransmitted";
	}

	fr_assert(!u->status_check);

	if (!u->proxied) {
		RDEBUG("%s request.  Expecting response within %pVs", action,
		       fr_box_time_delta(u->retry.rt));

	} else {
		/*
		 *	If the packet doesn't get a response,
		 *	then bio_request_free() will notice, and run conn_zombie()
		 */
		RDEBUG("%s request.  Relying on NAS to perform more retransmissions", action);
	}
}

/** Deal with Protocol-Error replies, and possible negotiation
 *
 */
static void protocol_error_reply(bio_request_t *u, bio_result_t *r, bio_handle_t *h)
{
	bool	  	error_601 = false;
	uint32_t  	response_length = 0;
	uint8_t const	*attr, *end;

	end = h->buffer + fr_nbo_to_uint16(h->buffer + 2);

	for (attr = h->buffer + RADIUS_HEADER_LENGTH;
	     attr < end;
	     attr += attr[1]) {
		/*
		 *	Error-Cause = Response-Too-Big
		 */
		if ((attr[0] == attr_error_cause->attr) && (attr[1] == 6)) {
			uint32_t error;

			memcpy(&error, attr + 2, 4);
			error = ntohl(error);
			if (error == 601) error_601 = true;
			continue;
		}

		/*
		 *	The other end wants us to increase our Response-Length
		 */
		if ((attr[0] == attr_response_length->attr) && (attr[1] == 6)) {
			memcpy(&response_length, attr + 2, 4);
			continue;
		}

		/*
		 *	Protocol-Error packets MUST contain an
		 *	Original-Packet-Code attribute.
		 *
		 *	The attribute containing the
		 *	Original-Packet-Code is an extended
		 *	attribute.
		 */
		if (attr[0] != attr_extended_attribute_1->attr) continue;

			/*
			 *	ATTR + LEN + EXT-Attr + uint32
			 */
			if (attr[1] != 7) continue;

			/*
			 *	See if there's an Original-Packet-Code.
			 */
			if (attr[2] != (uint8_t)attr_original_packet_code->attr) continue;

			/*
			 *	Has to be an 8-bit number.
			 */
			if ((attr[3] != 0) ||
			    (attr[4] != 0) ||
			    (attr[5] != 0)) {
				if (r) r->rcode = RLM_MODULE_FAIL;
				return;
			}

			/*
			 *	The value has to match.  We don't
			 *	currently multiplex different codes
			 *	with the same IDs on connections.  So
			 *	this check is just for RFC compliance,
			 *	and for sanity.
			 */
			if (attr[6] != u->code) {
				if (r) r->rcode = RLM_MODULE_FAIL;
				return;
			}
	}

	/*
	 *	Error-Cause = Response-Too-Big
	 *
	 *	The other end says it needs more room to send it's response
	 *
	 *	Limit it to reasonable values.
	 */
	if (error_601 && response_length && (response_length > h->buflen)) {
		if (response_length < 4096) response_length = 4096;
		if (response_length > 65535) response_length = 65535;

		DEBUG("%s - Increasing buffer size to %u for connection %s", h->module_name, response_length, h->fd_info->name);

		/*
		 *	Make sure to copy the packet over!
		 */
		attr = h->buffer;
		h->buflen = response_length;
		MEM(h->buffer = talloc_array(h, uint8_t, h->buflen));

		memcpy(h->buffer, attr, end - attr);
	}

	/*
	 *	fail - something went wrong internally, or with the connection.
	 *	invalid - wrong response to packet
	 *	handled - best remaining alternative :(
	 *
	 *	i.e. if the response is NOT accept, reject, whatever,
	 *	then we shouldn't allow the caller to do any more
	 *	processing of this packet.  There was a protocol
	 *	error, and the response is valid, but not useful for
	 *	anything.
	 */
	if (r) r->rcode = RLM_MODULE_HANDLED;
}


/** Handle retries for a status check
 *
 */
static void status_check_next(UNUSED fr_event_list_t *el, UNUSED fr_time_t now, void *uctx)
{
	trunk_connection_t	*tconn = talloc_get_type_abort(uctx, trunk_connection_t);
	bio_handle_t		*h = talloc_get_type_abort(tconn->conn->h, bio_handle_t);

	if (trunk_request_enqueue_on_conn(&h->status_r->treq, tconn, h->status_request,
					     h->status_u, h->status_r, true) != TRUNK_ENQUEUE_OK) {
		trunk_connection_signal_reconnect(tconn, CONNECTION_FAILED);
	}
}


/** Deal with replies replies to status checks and possible negotiation
 *
 */
static void status_check_reply(trunk_request_t *treq, fr_time_t now)
{
	bio_handle_t		*h = talloc_get_type_abort(treq->tconn->conn->h, bio_handle_t);
	rlm_radius_t const 	*inst = h->inst;
	bio_request_t		*u = talloc_get_type_abort(treq->preq, bio_request_t);
	bio_result_t		*r = talloc_get_type_abort(treq->rctx, bio_result_t);

	fr_assert(treq->preq == h->status_u);
	fr_assert(treq->rctx == h->status_r);

	r->treq = NULL;

	/*
	 *	@todo - do other negotiation and signaling.
	 */
	if (h->buffer[0] == FR_RADIUS_CODE_PROTOCOL_ERROR) protocol_error_reply(u, NULL, h);

	if (u->num_replies < inst->num_answers_to_alive) {
		DEBUG("Received %u / %u replies for status check, on connection - %s",
		      u->num_replies, inst->num_answers_to_alive, h->fd_info->name);
		DEBUG("Next status check packet will be in %pVs", fr_box_time_delta(fr_time_sub(u->retry.next, now)));

		/*
		 *	Set the timer for the next retransmit.
		 */
		if (fr_event_timer_at(h, h->thread->el, &u->ev, u->retry.next, status_check_next, treq->tconn) < 0) {
			trunk_connection_signal_reconnect(treq->tconn, CONNECTION_FAILED);
		}
		return;
	}

	DEBUG("Received enough replies to status check, marking connection as active - %s", h->fd_info->name);

	/*
	 *	Set the "last idle" time to now, so that we don't
	 *	restart zombie_period until sufficient time has
	 *	passed.
	 */
	h->last_idle = fr_time();

	/*
	 *	Reset retry interval and retransmission counters
	 *	also frees u->ev.
	 */
	status_check_reset(h, u);
	trunk_connection_signal_active(treq->tconn);
}

CC_NO_UBSAN(function) /* UBSAN: false positive - public vs private connection_t trips --fsanitize=function*/
static void request_demux(UNUSED fr_event_list_t *el, trunk_connection_t *tconn, connection_t *conn, UNUSED void *uctx)
{
	bio_handle_t		*h = talloc_get_type_abort(conn->h, bio_handle_t);

	DEBUG3("%s - Reading data for connection %s", h->module_name, h->fd_info->name);

	while (true) {
		ssize_t			slen;

		trunk_request_t	*treq;
		request_t		*request;
		bio_request_t		*u;
		bio_result_t		*r;
		radius_track_entry_t	*rr;
		decode_fail_t		reason;
		uint8_t			code = 0;
		fr_pair_list_t		reply;
		fr_pair_t		*vp;

		fr_time_t		now;

		fr_pair_list_init(&reply);

		/*
		 *	Drain the socket of all packets.  If we're busy, this
		 *	saves a round through the event loop.  If we're not
		 *	busy, a few extra system calls don't matter.
		 */
		slen = read(h->fd, h->buffer, h->buflen);
		if (slen == 0) return;

		if (slen < 0) {
			if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) return;

			ERROR("%s - Failed reading response from socket: %s",
			      h->module_name, fr_syserror(errno));
			trunk_connection_signal_reconnect(tconn, CONNECTION_FAILED);
			return;
		}

		if (slen < RADIUS_HEADER_LENGTH) {
			ERROR("%s - Packet too short, expected at least %zu bytes got %zd bytes",
			      h->module_name, (size_t)RADIUS_HEADER_LENGTH, slen);
			continue;
		}

		/*
		 *	Note that we don't care about packet codes.  All
		 *	packet codes share the same ID space.
		 */
		rr = radius_track_entry_find(h->tt, h->buffer[1], NULL);
		if (!rr) {
			WARN("%s - Ignoring reply with ID %i that arrived too late",
			     h->module_name, h->buffer[1]);
			continue;
		}

		treq = talloc_get_type_abort(rr->uctx, trunk_request_t);
		request = treq->request;
		fr_assert(request != NULL);
		u = talloc_get_type_abort(treq->preq, bio_request_t);
		r = talloc_get_type_abort(treq->rctx, bio_result_t);

		/*
		 *	Validate and decode the incoming packet
		 */

		if (!check(h, &slen)) {
			RWARN("Ignoring malformed packet");
			continue;
		}

		reason = decode(request->reply_ctx, &reply, &code, h, request, u, rr->vector, h->buffer, (size_t)slen);
		if (reason != DECODE_FAIL_NONE) continue;

		/*
		 *	Only valid packets are processed
		 *	Otherwise an attacker could perform
		 *	a DoS attack against the proxying servers
		 *	by sending fake responses for upstream
		 *	servers.
		 */
		h->last_reply = now = fr_time();

		/*
		 *	Status-Server can have any reply code, we don't care
		 *	what it is.  So long as it's signed properly, we
		 *	accept it.  This flexibility is because we don't
		 *	expose Status-Server to the admins.  It's only used by
		 *	this module for internal signalling.
		 */
		if (u == h->status_u) {
			fr_pair_list_free(&reply);	/* Probably want to pass this to status_check_reply? */
			status_check_reply(treq, now);
			trunk_request_signal_complete(treq);
			continue;
		}

		/*
		 *	Handle any state changes, etc. needed by receiving a
		 *	Protocol-Error reply packet.
		 *
		 *	Protocol-Error is permitted as a reply to any
		 *	packet.
		 */
		switch (code) {
		case FR_RADIUS_CODE_PROTOCOL_ERROR:
			protocol_error_reply(u, r, h);
			break;

		default:
			break;
		}

		/*
		 *	Mark up the request as being an Access-Challenge, if
		 *	required.
		 *
		 *	We don't do this for other packet types, because the
		 *	ok/fail nature of the module return code will
		 *	automatically result in it the parent request
		 *	returning an ok/fail packet code.
		 */
		if ((u->code == FR_RADIUS_CODE_ACCESS_REQUEST) && (code == FR_RADIUS_CODE_ACCESS_CHALLENGE)) {
			vp = fr_pair_find_by_da(&request->reply_pairs, NULL, attr_packet_type);
			if (!vp) {
				MEM(vp = fr_pair_afrom_da(request->reply_ctx, attr_packet_type));
				vp->vp_uint32 = FR_RADIUS_CODE_ACCESS_CHALLENGE;
				fr_pair_append(&request->reply_pairs, vp);
			}
		}

		/*
		 *	Delete Proxy-State attributes from the reply.
		 */
		fr_pair_delete_by_da(&reply, attr_proxy_state);

		/*
		 *	If the reply has Message-Authenticator, then over-ride its value with all zeros, so
		 *	that we don't confuse anyone reading the debug output.
		 */
		if ((vp = fr_pair_find_by_da(&reply, NULL, attr_message_authenticator)) != NULL) {
			(void) fr_pair_value_memdup(vp, (uint8_t const *) "", 1, false);
		}

		treq->request->reply->code = code;
		r->rcode = radius_code_to_rcode[code];
		fr_pair_list_append(&request->reply_pairs, &reply);
		trunk_request_signal_complete(treq);
	}
}

/** Remove the request from any tracking structures
 *
 * Frees encoded packets if the request is being moved to a new connection
 */
static void request_cancel(UNUSED connection_t *conn, void *preq_to_reset,
			   trunk_cancel_reason_t reason, UNUSED void *uctx)
{
	bio_request_t	*u = talloc_get_type_abort(preq_to_reset, bio_request_t);

	/*
	 *	Request has been requeued on the same
	 *	connection due to timeout or DUP signal.  We
	 *	keep the same packet to avoid re-encoding it.
	 */
	if (reason == TRUNK_CANCEL_REASON_REQUEUE) {
		/*
		 *	Delete the request_timeout
		 *
		 *	Note: There might not be a request timeout
		 *	set in the case where the request was
		 *	queued for sendmmsg but never actually
		 *	sent.
		 */
		if (u->ev) (void) fr_event_timer_delete(&u->ev);
	}

	/*
	 *      Other cancellations are dealt with by
	 *      request_conn_release as the request is removed
	 *	from the trunk.
	 */
}

/** Clear out anything associated with the handle from the request
 *
 */
static void request_conn_release(connection_t *conn, void *preq_to_reset, UNUSED void *uctx)
{
	bio_request_t		*u = talloc_get_type_abort(preq_to_reset, bio_request_t);
	bio_handle_t		*h = talloc_get_type_abort(conn->h, bio_handle_t);

	if (u->ev) (void)fr_event_timer_delete(&u->ev);
	if (u->packet) bio_request_reset(u);

	if (h->inst->mode == RLM_RADIUS_MODE_REPLICATE) return;

	u->num_replies = 0;

	/*
	 *	If there are no outstanding tracking entries
	 *	allocated then the connection is "idle".
	 */
	if (!h->tt || (h->tt->num_requests == 0)) h->last_idle = fr_time();
}

/** Write out a canned failure
 *
 */
static void request_fail(request_t *request, void *preq, void *rctx,
			 NDEBUG_UNUSED trunk_request_state_t state, UNUSED void *uctx)
{
	bio_result_t		*r = talloc_get_type_abort(rctx, bio_result_t);
	bio_request_t		*u = talloc_get_type_abort(preq, bio_request_t);

	fr_assert(!u->rr && !u->packet && fr_pair_list_empty(&u->extra) && !u->ev);	/* Dealt with by request_conn_release */

	fr_assert(state != TRUNK_REQUEST_STATE_INIT);

	if (u->status_check) return;

	r->rcode = RLM_MODULE_FAIL;
	r->treq = NULL;

	unlang_interpret_mark_runnable(request);
}

/** Response has already been written to the rctx at this point
 *
 */
static void request_complete(request_t *request, void *preq, void *rctx, UNUSED void *uctx)
{
	bio_result_t		*r = talloc_get_type_abort(rctx, bio_result_t);
	bio_request_t		*u = talloc_get_type_abort(preq, bio_request_t);

	fr_assert(!u->rr && !u->packet && fr_pair_list_empty(&u->extra) && !u->ev);	/* Dealt with by request_conn_release */

	if (u->status_check) return;

	r->treq = NULL;

	unlang_interpret_mark_runnable(request);
}

/** Explicitly free resources associated with the protocol request
 *
 */
static void request_free(UNUSED request_t *request, void *preq_to_free, UNUSED void *uctx)
{
	bio_request_t		*u = talloc_get_type_abort(preq_to_free, bio_request_t);

	fr_assert(!u->rr && !u->packet && fr_pair_list_empty(&u->extra) && !u->ev);	/* Dealt with by request_conn_release */

	/*
	 *	Don't free status check requests.
	 */
	if (u->status_check) return;

	talloc_free(u);
}

/** Resume execution of the request, returning the rcode set during trunk execution
 *
 */
static unlang_action_t mod_resume(rlm_rcode_t *p_result, module_ctx_t const *mctx, UNUSED request_t *request)
{
	bio_result_t	*r = talloc_get_type_abort(mctx->rctx, bio_result_t);
	rlm_rcode_t	rcode = r->rcode;

	talloc_free(r);

	RETURN_MODULE_RCODE(rcode);
}

static void mod_signal(module_ctx_t const *mctx, UNUSED request_t *request, fr_signal_t action)
{
	rlm_radius_t const	*inst = talloc_get_type_abort_const(mctx->mi->data, rlm_radius_t);

	bio_result_t		*r = talloc_get_type_abort(mctx->rctx, bio_result_t);
	bio_handle_t		*h;

	/*
	 *	We received a duplicate packet, but we're not doing
	 *	synchronous proxying.  Ignore the dup, and rely on the
	 *	IO submodule to time it's own retransmissions.
	 */
	if ((action == FR_SIGNAL_DUP) && (inst->mode != RLM_RADIUS_MODE_PROXY)) return;

	/*
	 *	If we don't have a treq associated with the
	 *	rctx it's likely because the request was
	 *	scheduled, but hasn't yet been resumed, and
	 *	has received a signal, OR has been resumed
	 *	and immediately cancelled as the event loop
	 *	is exiting, in which case
	 *	unlang_request_is_scheduled will return false
	 *	(don't use it).
	 */
	if (!r->treq) {
		talloc_free(r);
		return;
	}

	switch (action) {
	/*
	 *	The request is being cancelled, tell the
	 *	trunk so it can clean up the treq.
	 */
	case FR_SIGNAL_CANCEL:
		trunk_request_signal_cancel(r->treq);
		r->treq = NULL;
		talloc_free(r);		/* Should be freed soon anyway, but better to be explicit */
		return;

	/*
	 *	Requeue the request on the same connection
	 *      causing a "retransmission" if the request
	 *	has already been sent out.
	 */
	case FR_SIGNAL_DUP:
		h = r->treq->tconn->conn->h;

		if (h->fd_info->write_blocked) {
			RDEBUG("IO is blocked - suppressing retransmission");
			return;
		}
		r->is_retry = true;

		/*
		 *	We are doing synchronous proxying, retransmit
		 *	the current request on the same connection.
		 *
		 *	If it's zombie, we still resend it.  If the
		 *	connection is dead, then a callback will move
		 *	this request to a new connection.
		 */
		mod_write(request, r->treq, h);
		return;

	default:
		return;
	}
}

#ifndef NDEBUG
/** Free a bio_result_t
 *
 * Allows us to set break points for debugging.
 */
static int _bio_result_free(bio_result_t *r)
{
	trunk_request_t	*treq;
	bio_request_t		*u;

	if (!r->treq) return 0;

	treq = talloc_get_type_abort(r->treq, trunk_request_t);
	u = talloc_get_type_abort(treq->preq, bio_request_t);

	fr_assert_msg(!u->ev, "bio_result_t freed with active timer");

	return 0;
}
#endif

/** Free a bio_request_t
 */
static int _bio_request_free(bio_request_t *u)
{
	if (u->ev) (void) fr_event_timer_delete(&u->ev);

	fr_assert(u->rr == NULL);

	return 0;
}

static unlang_action_t mod_enqueue(rlm_rcode_t *p_result, rlm_radius_t const *inst, void *thread, request_t *request)
{
	bio_thread_t			*t = talloc_get_type_abort(thread, bio_thread_t);
	bio_result_t			*r;
	bio_request_t			*u;
	trunk_request_t			*treq;
	fr_retry_config_t const		*retry_config;

	fr_assert(request->packet->code > 0);
	fr_assert(request->packet->code < FR_RADIUS_CODE_MAX);

	if (request->packet->code == FR_RADIUS_CODE_STATUS_SERVER) {
		RWDEBUG("Status-Server is reserved for internal use, and cannot be sent manually.");
		RETURN_MODULE_NOOP;
	}

	treq = trunk_request_alloc(t->trunk, request);
	if (!treq) RETURN_MODULE_FAIL;

	MEM(r = talloc_zero(request, bio_result_t));
#ifndef NDEBUG
	talloc_set_destructor(r, _bio_result_free);
#endif

	/*
	 *	Can't use compound literal - const issues.
	 */
	MEM(u = talloc_zero(treq, bio_request_t));
	u->code = request->packet->code;
	u->priority = request->async->priority;
	u->recv_time = request->async->recv_time;
	fr_pair_list_init(&u->extra);

	u->retry.count = 1;

	r->rcode = RLM_MODULE_FAIL;

	/*
	 *	Make sure that we print out the actual encoded value
	 *	of the Message-Authenticator attribute.  If the caller
	 *	asked for one, delete theirs (which has a bad value),
	 *	and remember to add one manually when we encode the
	 *	packet.  This is the only editing we do on the input
	 *	request.
	 *
	 *	@todo - don't edit the input packet!
	 */
	if (fr_pair_find_by_da(&request->request_pairs, NULL, attr_message_authenticator)) {
		u->require_message_authenticator = true;
		pair_delete_request(attr_message_authenticator);
	}

	switch(trunk_request_enqueue(&treq, t->trunk, request, u, r)) {
	case TRUNK_ENQUEUE_OK:
	case TRUNK_ENQUEUE_IN_BACKLOG:
		break;

	case TRUNK_ENQUEUE_NO_CAPACITY:
		REDEBUG("Unable to queue packet - connections at maximum capacity");
	fail:
		fr_assert(!u->rr && !u->packet);	/* Should not have been fed to the muxer */
		trunk_request_free(&treq);		/* Return to the free list */
		talloc_free(r);
		RETURN_MODULE_FAIL;

	case TRUNK_ENQUEUE_DST_UNAVAILABLE:
		REDEBUG("All destinations are down - cannot send packet");
		goto fail;

	case TRUNK_ENQUEUE_FAIL:
		REDEBUG("Unable to queue packet");
		goto fail;
	}

	r->treq = treq;	/* Remember for signalling purposes */
	fr_assert(treq->rctx == r);

	talloc_set_destructor(u, _bio_request_free);

	/*
	 *	Figure out if we're originating the packet or proxying it.  And also figure out if we have to
	 *	retry.
	 */
	switch (inst->mode) {
	case RLM_RADIUS_MODE_INVALID:
		RETURN_MODULE_FAIL;

		/*
		 *	We originate this packet if it was taken from the detail module, which doesn't have a
		 *	real client.  @todo - do a better check here.
		 *
		 *	We originate this packet if the parent request is not compatible with this one
		 *	(i.e. it's from a different protocol).
		 *
		 *	We originate the packet if the parent is from the same dictionary, but has a different
		 *	packet code.  This lets us receive Accounting-Request, and originate
		 *	Disconnect-Request.
		 */
	case RLM_RADIUS_MODE_PROXY:
		if (!request->parent) {
			u->proxied = (request->client->cs != NULL);

		} else if (!fr_dict_compatible(request->parent->dict, request->dict)) {
			u->proxied = false;

		} else {
			u->proxied = (request->parent->packet->code == request->packet->code);
		}

		/*
		 *	Proxied packets get a final timeout, as we retry only on DUP packets.
		 */
		if (u->proxied) goto timeout_retry;

		FALL_THROUGH;

		/*
		 *	Client packets (i.e. packets we originate) get retries for UDP.  And no retries for TCP.
		 */
	case RLM_RADIUS_MODE_CLIENT:
		if (inst->fd_config.socket_type == SOCK_DGRAM) {
			retry_config = &inst->retry[u->code];
			break;
		}
		FALL_THROUGH;

		/*
		 *	Replicated packets are never retried, but they have a timeout if the socket isn't
		 *	ready for writing.
		 */
	case RLM_RADIUS_MODE_REPLICATE:
	timeout_retry:
		retry_config = &inst->timeout_retry;
		break;
	}

	/*
	 *	The event loop will take care of demux && sending the
	 *	packet, along with any retransmissions.
	 */
	return unlang_module_yield_to_retry(request, mod_resume, mod_retry, mod_signal, 0, r, retry_config);
}

/** Instantiate thread data for the submodule.
 *
 */
static int mod_thread_instantiate(module_thread_inst_ctx_t const *mctx)
{
	rlm_radius_t		*inst = talloc_get_type_abort(mctx->mi->data, rlm_radius_t);
	bio_thread_t			*thread = talloc_get_type_abort(mctx->thread, bio_thread_t);

	static trunk_io_funcs_t	io_funcs = {
						.connection_alloc = thread_conn_alloc,
						.connection_notify = thread_conn_notify,
						.request_prioritise = request_prioritise,
						.request_mux = request_mux,
						.request_demux = request_demux,
						.request_conn_release = request_conn_release,
						.request_complete = request_complete,
						.request_fail = request_fail,
						.request_cancel = request_cancel,
						.request_free = request_free
					};

	thread->el = mctx->el;
	thread->inst = inst;
	thread->trunk = trunk_alloc(thread, mctx->el, &io_funcs,
				    &inst->trunk_conf, inst->name, thread, false);
	if (!thread->trunk) return -1;

	return 0;
}
