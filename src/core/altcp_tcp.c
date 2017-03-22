/**
 * @file
 * Application layered TCP connection API (to be used from TCPIP thread)\n
 * This interface mimics the tcp callback API to the application while preventing
 * direct linking (much like virtual functions).
 * This way, an application can make use of other application layer protocols
 * on top of TCP without knowing the details (e.g. TLS, proxy connection).
 *
 * This file contains the base implementation calling into tcp.
 */

/*
 * Copyright (c) 2017 Simon Goldschmidt
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Simon Goldschmidt <goldsimon@gmx.de>
 *
 */

#include "lwip/opt.h"

#if LWIP_ALTCP /* don't build if not configured for use in lwipopts.h */

#include "lwip/altcp.h"
#include "lwip/priv/altcp_priv.h"
#include "lwip/tcp.h"
#include "lwip/mem.h"

#include <string.h>

/* Variable prototype, the actual declaration is at the end of this file
   since it contains pointers to static functions declared here */
extern const struct altcp_functions altcp_tcp_functions;

static void altcp_tcp_setup(struct altcp_pcb *conn, struct tcp_pcb *tpcb);

/* callback functions for TCP */
static err_t
altcp_tcp_accept(void *arg, struct tcp_pcb *new_tpcb, err_t err)
{
  struct altcp_pcb *listen_conn = (struct altcp_pcb *)arg;
  if (listen_conn && listen_conn->accept) {
    /* create a new altcp_conn to pass to the next 'accept' callback */
    struct altcp_pcb *new_conn = altcp_alloc();
    if (new_conn == NULL) {
      return ERR_MEM;
    }
    altcp_tcp_setup(new_conn, new_tpcb);
    return listen_conn->accept(listen_conn->arg, new_conn, err);
  }
  return ERR_ARG;
}

static err_t
altcp_tcp_connected(void *arg, struct tcp_pcb *tpcb, err_t err)
{
  struct altcp_pcb *conn = (struct altcp_pcb *)arg;
  if (conn) {
    LWIP_ASSERT("pcb mismatch", conn->inner_conn == (struct altcp_pcb *)tpcb);
    if (conn->connected) {
      return conn->connected(conn->arg, conn, err);
    }
  }
  return ERR_OK;
}

static err_t
altcp_tcp_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err)
{
  struct altcp_pcb *conn = (struct altcp_pcb *)arg;
  if (conn) {
    LWIP_ASSERT("pcb mismatch", conn->inner_conn == (struct altcp_pcb *)tpcb);
    if (conn->recv) {
      return conn->recv(conn->arg, conn, p, err);
    }
  }
  if (p != NULL) {
    /* prevent memory leaks */
    pbuf_free(p);
  }
  return ERR_OK;
}

static err_t
altcp_tcp_sent(void *arg, struct tcp_pcb *tpcb, u16_t len)
{
  struct altcp_pcb *conn = (struct altcp_pcb *)arg;
  if (conn) {
    LWIP_ASSERT("pcb mismatch", conn->inner_conn == (struct altcp_pcb *)tpcb);
    if (conn->sent) {
      return conn->sent(conn->arg, conn, len);
    }
  }
  return ERR_OK;
}

static err_t
altcp_tcp_poll(void *arg, struct tcp_pcb *tpcb)
{
  struct altcp_pcb *conn = (struct altcp_pcb *)arg;
  if (conn) {
    LWIP_ASSERT("pcb mismatch", conn->inner_conn == (struct altcp_pcb *)tpcb);
    if (conn->poll) {
      return conn->poll(conn->arg, conn);
    }
  }
  return ERR_OK;
}

static void
altcp_tcp_err(void *arg, err_t err)
{
  struct altcp_pcb *conn = (struct altcp_pcb *)arg;
  if (conn) {
    if (conn->err) {
      conn->err(conn->arg, err);
    }
  }
}

/* setup functions */
static void
altcp_tcp_setup_callbacks(struct altcp_pcb *conn, struct tcp_pcb *tpcb)
{
  tcp_arg(tpcb, conn);
  tcp_recv(tpcb, altcp_tcp_recv);
  tcp_sent(tpcb, altcp_tcp_sent);
  tcp_err(tpcb, altcp_tcp_err);
  /* tcp_poll is set when interval is set by application */
  /* listen is set totally different :-) */
}

static void
altcp_tcp_setup(struct altcp_pcb *conn, struct tcp_pcb *tpcb)
{
  altcp_tcp_setup_callbacks(conn, tpcb);
  conn->inner_conn = (struct altcp_pcb *)tpcb;
  conn->fns = &altcp_tcp_functions;
}

struct altcp_pcb *
altcp_tcp_new_ip_type(u8_t ip_type)
{
  /* FIXME: pool alloc */
  struct altcp_pcb *ret = altcp_alloc();
  if (ret != NULL) {
    struct tcp_pcb *tpcb = tcp_new_ip_type(ip_type);
    if (tpcb != NULL) {
      altcp_tcp_setup(ret, tpcb);
    } else {
      /* tcp_pcb allocation failed -> free the altcp_pcb too */
      altcp_free(ret);
      ret = NULL;
    }
  }
  return ret;
}


/* "virtual" functions calling into tcp */
static void
altcp_tcp_set_poll(struct altcp_pcb *conn, u8_t interval)
{
  if (conn != NULL) {
    struct tcp_pcb *pcb = (struct tcp_pcb *)conn->inner_conn;
    tcp_poll(pcb, altcp_tcp_poll, interval);
  }
}

static void
altcp_tcp_recved(struct altcp_pcb *conn, u16_t len)
{
  if (conn != NULL) {
    struct tcp_pcb *pcb = (struct tcp_pcb *)conn->inner_conn;
    tcp_recved(pcb, len);
  }
}

static err_t
altcp_tcp_bind(struct altcp_pcb *conn, const ip_addr_t *ipaddr, u16_t port)
{
  struct tcp_pcb *pcb;
  if (conn == NULL) {
    return ERR_VAL;
  }
  pcb = (struct tcp_pcb *)conn->inner_conn;
  return tcp_bind(pcb, ipaddr, port);
}

static err_t
altcp_tcp_connect(struct altcp_pcb *conn, const ip_addr_t *ipaddr, u16_t port, altcp_connected_fn connected)
{
  struct tcp_pcb *pcb;
  if (conn == NULL) {
    return ERR_VAL;
  }
  conn->connected = connected;
  pcb = (struct tcp_pcb *)conn->inner_conn;
  return tcp_connect(pcb, ipaddr, port, altcp_tcp_connected);
}

static struct altcp_pcb *
altcp_tcp_listen(struct altcp_pcb *conn, u8_t backlog, err_t *err)
{
  struct tcp_pcb *pcb;
  struct tcp_pcb *lpcb;
  if (conn == NULL) {
    return NULL;
  }
  pcb = (struct tcp_pcb *)conn->inner_conn;
  lpcb = tcp_listen_with_backlog_and_err(pcb, backlog, err);
  if (lpcb != NULL) {
    conn->inner_conn = (struct altcp_pcb *)lpcb;
    tcp_accept(lpcb, altcp_tcp_accept);
    return conn;
  }
  return NULL;
}

static void
altcp_tcp_abort(struct altcp_pcb *conn)
{
  if (conn != NULL) {
    struct tcp_pcb *pcb = (struct tcp_pcb *)conn->inner_conn;
    tcp_abort(pcb);
  }
}

static err_t
altcp_tcp_close(struct altcp_pcb *conn)
{
  struct tcp_pcb *pcb;
  if (conn == NULL) {
    return ERR_VAL;
  }
  pcb = (struct tcp_pcb *)conn->inner_conn;
  return tcp_close(pcb);
}

static err_t
altcp_tcp_shutdown(struct altcp_pcb *conn, int shut_rx, int shut_tx)
{
  struct tcp_pcb *pcb;
  if (conn == NULL) {
    return ERR_VAL;
  }
  pcb = (struct tcp_pcb *)conn->inner_conn;
  return tcp_shutdown(pcb, shut_rx, shut_tx);
}

static err_t
altcp_tcp_write(struct altcp_pcb *conn, const void *dataptr, u16_t len, u8_t apiflags)
{
  struct tcp_pcb *pcb;
  if (conn == NULL) {
    return ERR_VAL;
  }
  pcb = (struct tcp_pcb *)conn->inner_conn;
  return tcp_write(pcb, dataptr, len, apiflags);
}

static err_t
altcp_tcp_output(struct altcp_pcb *conn)
{
  struct tcp_pcb *pcb;
  if (conn == NULL) {
    return ERR_VAL;
  }
  pcb = (struct tcp_pcb *)conn->inner_conn;
  return tcp_output(pcb);
}

static u16_t
altcp_tcp_mss(struct altcp_pcb *conn)
{
  struct tcp_pcb *pcb;
  if (conn == NULL) {
    return 0;
  }
  pcb = (struct tcp_pcb *)conn->inner_conn;
  return tcp_mss(pcb);
}

static u16_t
altcp_tcp_sndbuf(struct altcp_pcb *conn)
{
  struct tcp_pcb *pcb;
  if (conn == NULL) {
    return 0;
  }
  pcb = (struct tcp_pcb *)conn->inner_conn;
  return tcp_sndbuf(pcb);
}

static u16_t
altcp_tcp_sndqueuelen(struct altcp_pcb *conn)
{
  struct tcp_pcb *pcb;
  if (conn == NULL) {
    return 0;
  }
  pcb = (struct tcp_pcb *)conn->inner_conn;
  return tcp_sndqueuelen(pcb);
}

static void
altcp_tcp_setprio(struct altcp_pcb *conn, u8_t prio)
{
  if (conn != NULL) {
    struct tcp_pcb *pcb = (struct tcp_pcb *)conn->inner_conn;
    tcp_setprio(pcb, prio);
  }
}

static void
altcp_tcp_dealloc(struct altcp_pcb *conn)
{
  LWIP_UNUSED_ARG(conn);
  /* no private state to clean up */
}

err_t
altcp_tcp_get_tcp_addrinfo(struct altcp_pcb *conn, int local, ip_addr_t *addr, u16_t *port)
{
  if (conn) {
    struct tcp_pcb *pcb = (struct tcp_pcb *)conn->inner_conn;
    return tcp_tcp_get_tcp_addrinfo(pcb, local, addr, port);
  }
  return ERR_VAL;
}

#ifdef LWIP_DEBUG
enum tcp_state
altcp_tcp_dbg_get_tcp_state(struct altcp_pcb *conn)
{
  if (conn) {
    struct tcp_pcb *pcb = (struct tcp_pcb *)conn->inner_conn;
    if (pcb) {
      return pcb->state;
    }
  }
  return CLOSED;
}
#endif
const struct altcp_functions altcp_tcp_functions = {
  altcp_tcp_set_poll,
  altcp_tcp_recved,
  altcp_tcp_bind,
  altcp_tcp_connect,
  altcp_tcp_listen,
  altcp_tcp_abort,
  altcp_tcp_close,
  altcp_tcp_shutdown,
  altcp_tcp_write,
  altcp_tcp_output,
  altcp_tcp_mss,
  altcp_tcp_sndbuf,
  altcp_tcp_sndqueuelen,
  altcp_tcp_setprio,
  altcp_tcp_dealloc,
  altcp_tcp_get_tcp_addrinfo
#ifdef LWIP_DEBUG
  ,altcp_tcp_dbg_get_tcp_state
#endif
};

#endif /* LWIP_ALTCP */