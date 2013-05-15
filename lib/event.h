/* Copyright 2011,2013 NORDUnet A/S. All rights reserved.
   See LICENSE for licensing information. */

void event_on_disconnect (struct rs_connection *conn);
int event_on_connect_orig (struct rs_connection *conn, struct rs_message *msg);
int event_on_connect_term (struct rs_connection *conn, struct rs_message *msg);
int event_loopbreak (struct rs_connection *conn);
int event_init_eventbase (struct rs_conn_base *connbase);
int event_init_socket (struct rs_conn_base *connbase, struct rs_peer *p);
int event_init_bufferevent (struct rs_connection *conn);
void event_do_connect (struct rs_connection *conn);
void event_conn_timeout_cb (int fd, short event, void *data);
void event_retransmit_timeout_cb (int fd, short event, void *data);
