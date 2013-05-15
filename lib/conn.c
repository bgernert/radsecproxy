/* Copyright 2010,2011,2013 NORDUnet A/S. All rights reserved.
   See LICENSE for licensing information. */

#if defined HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <assert.h>
#include <event2/event.h>
#include <event2/bufferevent.h>
#include <radsec/radsec.h>
#include <radsec/radsec-impl.h>
#include "err.h"
#include "debug.h"
#include "conn.h"
#include "event.h"
#include "message.h"
#include "tcp.h"

int
conn_user_dispatch_p (const struct rs_connection *conn)
{
  assert (conn);

  return (conn->callbacks.connected_cb ||
	  conn->callbacks.disconnected_cb ||
	  conn->callbacks.received_cb ||
	  conn->callbacks.sent_cb);
}

int
conn_activate_timeout (struct rs_connection *conn)
{
  assert (conn);
  assert (conn->tev);
  assert (conn->base_.ctx->evb);
  if (conn->base_.timeout.tv_sec || conn->base_.timeout.tv_usec)
    {
      rs_debug (("%s: activating timer: %d.%d\n", __func__,
		 conn->base_.timeout.tv_sec, conn->base_.timeout.tv_usec));
      if (evtimer_add (conn->tev, &conn->base_.timeout))
	return rs_err_conn_push_fl (conn, RSE_EVENT, __FILE__, __LINE__,
				    "evtimer_add: %d", errno);
    }
  return RSE_OK;
}

int
conn_type_tls_p (const struct rs_connection *conn)
{
  return TO_BASE_CONN(conn)->transport == RS_CONN_TYPE_TLS
    || TO_BASE_CONN(conn)->transport == RS_CONN_TYPE_DTLS;
}

int
baseconn_type_datagram_p (const struct rs_conn_base *connbase)
{
  return connbase->transport == RS_CONN_TYPE_UDP
    || connbase->transport == RS_CONN_TYPE_DTLS;
}

int
baseconn_type_stream_p (const struct rs_conn_base *connbase)
{
  return connbase->transport == RS_CONN_TYPE_TCP
    || connbase->transport == RS_CONN_TYPE_TLS;
}

int
conn_cred_psk (const struct rs_connection *conn)
{
  assert (conn);
  assert (conn->active_peer);
  return conn->active_peer->transport_cred &&
    conn->active_peer->transport_cred->type == RS_CRED_TLS_PSK;
}

void
conn_init (struct rs_context *ctx, /* FIXME: rename connbase_init? */
           struct rs_conn_base *connbase,
           enum rs_conn_subtype type)
{
  switch (type)
    {
    case RS_CONN_OBJTYPE_BASE:
      connbase->magic = RS_CONN_MAGIC_BASE;
      break;
    case RS_CONN_OBJTYPE_GENERIC:
      connbase->magic = RS_CONN_MAGIC_GENERIC;
      break;
    case RS_CONN_OBJTYPE_LISTENER:
      connbase->magic = RS_CONN_MAGIC_LISTENER;
      break;
    default:
      assert ("invalid connection subtype" == NULL);
    }

  connbase->ctx = ctx;
  connbase->fd = -1;
}

int
conn_configure (struct rs_context *ctx, /* FIXME: rename conbbase_configure? */
                struct rs_conn_base *connbase,
                const char *config)
{
  if (config)
    {
      struct rs_realm *r = rs_conf_find_realm (ctx, config);
      if (r)
	{
	  connbase->realm = r;
	  connbase->peers = r->peers; /* FIXME: Copy instead?  */
#if 0
	  for (p = connbase->peers; p != NULL; p = p->next)
	    p->connbase = connbase;
#endif
	  connbase->timeout.tv_sec = r->timeout;
	  connbase->tryagain = r->retries;
	}
    }
#if 0  /* incoming connections don't have a realm (a config object), update: they do, but "somebody else" is setting this up <-- FIXME */
  if (connbase->realm == NULL)
    {
      struct rs_realm *r = rs_calloc (ctx, 1, sizeof (struct rs_realm));
      if (r == NULL)
        return rs_err_ctx_push_fl (ctx, RSE_NOMEM, __FILE__, __LINE__, NULL);
      r->next = ctx->realms;
      ctx->realms = connbase->realm = r;
    }
#else
  if (connbase->realm)
    connbase->transport = connbase->realm->type;
#endif

  return RSE_OK;
}

int
conn_add_read_event (struct rs_connection *conn, void *user_data)
{
  struct rs_conn_base *connbase = TO_BASE_CONN(conn);
  int err;

  assert(connbase);

  if (connbase->bev)		/* TCP (including TLS).  */
    {
      bufferevent_setwatermark (connbase->bev, EV_READ, RS_HEADER_LEN, 0);
      bufferevent_setcb (connbase->bev, tcp_read_cb, NULL, tcp_event_cb,
                         user_data);
      bufferevent_enable (connbase->bev, EV_READ);
    }
  else				/* UDP.  */
    {
      /* Put fresh message in user_data for the callback and enable the
	 read event.  */
      event_assign (connbase->rev, connbase->ctx->evb,
                    event_get_fd (connbase->rev), EV_READ,
                    event_get_callback (connbase->rev),
                    user_data);
      err = event_add (connbase->rev, NULL);
      if (err < 0)
	return rs_err_connbase_push_fl (connbase, RSE_EVENT, __FILE__, __LINE__,
                                        "event_add: %s",
                                        evutil_gai_strerror (err));

      /* Activate retransmission timer.  */
      conn_activate_timeout (conn);
    }

  return RSE_OK;
}

/** Return !=0 if \a conn is an originating connection, i.e. if its
    peer is a server. */
int
conn_originating_p (const struct rs_connection *conn)
{
  return conn->active_peer->type == RS_PEER_TYPE_SERVER;
}

int
baseconn_close (struct rs_conn_base *connbase)
{
  int err = 0;
  assert (connbase);

  rs_debug (("%s: closing fd %d\n", __func__, connbase->fd));

  err = evutil_closesocket (connbase->fd);
  if (err)
    err = rs_err_connbase_push (connbase, RSE_EVENT,
                                "evutil_closesocket: %d (%s)",
                                errno, strerror (errno));
  connbase->fd = -1;
  return err;
}

/* Public functions. */
int
rs_conn_create (struct rs_context *ctx,
		struct rs_connection **conn,
		const char *config)
{
  int err = RSE_OK;
  struct rs_connection *c = NULL;
  assert (ctx);

  c = rs_calloc (ctx, 1, sizeof (struct rs_connection));
  if (c == NULL)
    return rs_err_ctx_push_fl (ctx, RSE_NOMEM, __FILE__, __LINE__, NULL);
  conn_init (ctx, &c->base_, RS_CONN_OBJTYPE_GENERIC);
  err = conn_configure (ctx, &c->base_, config);
  if (err)
    goto errout;

  if (conn)
    *conn = c;
  return RSE_OK;

 errout:
  if (c)
    rs_free (ctx, c);
  return err;
}

void
rs_conn_set_type (struct rs_connection *conn, rs_conn_type_t type)
{
  assert (conn);
  assert (conn->base_.realm);
  conn->base_.realm->type = type;
}

int
rs_conn_add_listener (struct rs_connection *conn,
		      rs_conn_type_t type,
		      const char *hostname,
		      int port)
{
  return rs_err_conn_push_fl (conn, RSE_NOSYS, __FILE__, __LINE__, NULL);
}


int
rs_conn_disconnect (struct rs_connection *conn)
{
  int err = baseconn_close (TO_BASE_CONN (conn));
  conn->state = RS_CONN_STATE_UNDEFINED;
  return err;
}

int
rs_conn_destroy (struct rs_connection *conn)
{
  int err = 0;

  assert (conn);

  /* NOTE: conn->realm is owned by context.  */
  /* NOTE: conn->peers is owned by context.  */

  if (conn->state == RS_CONN_STATE_CONNECTED)
    err = rs_conn_disconnect (conn);

#if defined (RS_ENABLE_TLS)
  if (conn->tls_ssl) /* FIXME: Free SSL strucxt in rs_conn_disconnect?  */
    SSL_free (conn->tls_ssl);
  if (conn->tls_ctx)
    SSL_CTX_free (conn->tls_ctx);
#endif

  if (conn->tev)
    event_free (conn->tev);
  if (conn->base_.bev)
    bufferevent_free (conn->base_.bev);
  if (conn->base_.rev)
    event_free (conn->base_.rev);
  if (conn->base_.wev)
    event_free (conn->base_.wev);

  rs_free (conn->base_.ctx, conn);

  return err;
}

int
rs_conn_set_eventbase (struct rs_connection *conn, struct event_base *eb)
{
  return rs_err_conn_push_fl (conn, RSE_NOSYS, __FILE__, __LINE__, NULL);
}

void
rs_conn_set_callbacks (struct rs_connection *conn,
                       struct rs_conn_callbacks *cb,
                       void *user_data)
{
  assert (conn);
  TO_BASE_CONN(conn)->user_data = user_data;
  memcpy (&conn->callbacks, cb, sizeof (conn->callbacks));
}

void
rs_conn_del_callbacks (struct rs_connection *conn)
{
  assert (conn);
  memset (&conn->callbacks, 0, sizeof (conn->callbacks));
}

struct rs_conn_callbacks *
rs_conn_get_callbacks (struct rs_connection *conn)
{
  assert (conn);
  return &conn->callbacks;
}

int
rs_conn_select_peer (struct rs_connection *conn, const char *name)
{
  return rs_err_conn_push_fl (conn, RSE_NOSYS, __FILE__, __LINE__, NULL);
}

int
rs_conn_get_current_peer (struct rs_connection *conn,
			  const char *name,
			  size_t buflen)
{
  return rs_err_conn_push_fl (conn, RSE_NOSYS, __FILE__, __LINE__, NULL);
}

int
rs_conn_dispatch (struct rs_connection *conn)
{
  assert (conn);
  return event_base_loop (conn->base_.ctx->evb, EVLOOP_ONCE);
}

#if 0
struct event_base
*rs_conn_get_evb (const struct rs_connection *conn)
{
  assert (conn);
  return conn->evb;
}
#endif

int rs_conn_get_fd (struct rs_connection *conn)
{
  assert (conn);
  return conn->base_.fd;
}

static void
_rcb (struct rs_message *message, void *user_data)
{
  struct rs_message *msg = (struct rs_message *) user_data;
  assert (msg);
  assert (msg->conn);

  msg->flags |= RS_MESSAGE_RECEIVED;
  if (msg->conn->base_.bev)     /* TCP -- disable bufferevent. */
    bufferevent_disable (msg->conn->base_.bev, EV_WRITE|EV_READ);
  else                          /* UDP -- remove read event. */
    event_del (msg->conn->base_.rev);
}

int
rs_conn_receive_message (struct rs_connection *conn,
                         struct rs_message *req_msg,
                         struct rs_message **msg_out)
{
  int err = 0;
  struct rs_message *msg = NULL;

  assert (conn);
  assert (conn->base_.realm);
  assert (!conn_user_dispatch_p (conn)); /* Blocking mode only.  */

  if (rs_message_create (conn, &msg))
    return -1;

  assert (conn->base_.ctx->evb);
  assert (conn->base_.fd >= 0);

  conn->callbacks.received_cb = _rcb;
  conn->base_.user_data = msg;
  msg->flags &= ~RS_MESSAGE_RECEIVED;

  err = conn_add_read_event (conn, msg);
  if (err)
    return err;

  rs_debug (("%s: entering event loop\n", __func__));
  err = event_base_dispatch (conn->base_.ctx->evb);
  conn->callbacks.received_cb = NULL;
  if (err < 0)
    return rs_err_conn_push_fl (conn, RSE_EVENT, __FILE__, __LINE__,
				"event_base_dispatch: %s",
				evutil_gai_strerror (err));
  rs_debug (("%s: event loop done\n", __func__));

  if ((msg->flags & RS_MESSAGE_RECEIVED) == 0 /* No message. */
      || (req_msg
	  && message_verify_response (conn, msg, req_msg) != RSE_OK))
    {
      if (rs_err_conn_peek_code (conn) == RSE_OK)
        {
          /* No message and no error on the stack _should_ mean that the
             server hung up on us. */
          rs_err_conn_push (conn, RSE_DISCO, "no response");
        }
      return rs_err_conn_peek_code (conn);
    }

  if (msg_out)
    *msg_out = msg;
  return RSE_OK;
}

void
rs_conn_set_timeout(struct rs_connection *conn, struct timeval *tv)
{
  assert (conn);
  assert (tv);
  conn->base_.timeout = *tv;
}

struct rs_peer *
connbase_get_peers (const struct rs_conn_base *connbase)
{
  assert (connbase);
  return connbase->peers;
}
