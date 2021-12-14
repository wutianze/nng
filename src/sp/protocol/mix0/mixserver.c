//
// Copyright 2020 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2018 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

// now we do not support windows platform in mixserver
//#ifndef _WIN32

#include <stdlib.h>

#include "core/nng_impl.h"
#include "nng/protocol/mix0/mixserver.h"

/*for posix
#include <fcntl.h>
#include <poll.h>
bool poll_fd(int fd){
	struct pollfd pfd;
	pfd.fd      = fd;
	pfd.events  = POLLRDNORM;
	pfd.revents = 0;

	switch (poll(&pfd, 1, 0)) {
	case 0:
		return (false);
	case 1:
		return (true);
	}
	return false;
}*/

// Mix protocol provides the ability to deal with multi-interfaces scenario.
// Mix is like pair1_poly.

#ifdef NNG_ENABLE_STATS
#define BUMP_STAT(x) nni_stat_inc(x, 1)
#else
#define BUMP_STAT(x)
#endif

typedef struct mixserver_pipe mixserver_pipe;
typedef struct mixserver_sock mixserver_sock;

static void mixserver_sock_get_cb(void *);
static void mixserver_pipe_send_cb(void *);
static void mixserver_pipe_recv_cb(void *);
static void mixserver_pipe_get_cb(void *);
static void mixserver_pipe_put_cb(void *);
static void mixserver_pipe_fini(void *);
// mixserver_sock is our per-socket protocol private structure.
struct mixserver_sock {
	nni_sock      *sock;
	nni_msgq      *uwq;
	nni_msgq      *urq_urgent;
	nni_msgq      *urq_normal;
	nni_msgq      *urq_unimportant;
	/* replaced by adding tryget in msgqueue.c
	int            fd_urgent;
	int            fd_normal;
	int            fd_unimportant;
	*/

	// mtx is to protect id_map and list for pipes
	// for pipe close&start will make changes to them
	nni_mtx        mtx;	
	nni_id_map     pipes;

	//policy related
	int            recv_policy;
#ifdef NNG_ENABLE_STATS
	nni_stat_item  stat_mixserver;
	nni_stat_item  stat_reject_mismatch;
	nni_stat_item  stat_reject_already;
	nni_stat_item  stat_rx_malformed;
	nni_stat_item  stat_tx_malformed;
	nni_stat_item  stat_tx_drop;
#endif
};

// mixserver_pipe is our per-pipe protocol private structure.
struct mixserver_pipe {
	nni_pipe       *pipe;
	mixserver_sock       *pair;
	nni_msgq       *send_queue;
	nni_aio         aio_send;
	nni_aio         aio_recv;
	nni_aio         aio_get;
	nni_aio         aio_put;
};

static void
mixserver_sock_fini(void *arg)
{
	mixserver_sock *s = arg;

	nni_id_map_fini(&s->pipes);
	nni_msgq_fini(s->urq_urgent);
	nni_msgq_fini(s->urq_normal);
	nni_msgq_fini(s->urq_unimportant);
	nni_mtx_fini(&s->mtx);
}

#ifdef NNG_ENABLE_STATS
static void
mixserver_add_sock_stat(
    mixserver_sock *s, nni_stat_item *item, const nni_stat_info *info)
{
	nni_stat_init(item, info);
	nni_sock_add_stat(s->sock, item);
}
#endif

static void
mixserver_sock_open(void *arg)
{
	NNI_ARG_UNUSED(arg);
}

static void
mixserver_sock_close(void *arg)
{
	mixserver_sock *s = arg;
	nni_msgq_close(s->urq_urgent);
	nni_msgq_close(s->urq_normal);
	nni_msgq_close(s->urq_unimportant);
}

static int
mixserver_sock_init(void *arg, nni_sock *sock)
{
	mixserver_sock *s = arg;

	nni_id_map_init(&s->pipes, 0, 0, false);
	s->sock = sock;

	// Raw mode uses this.
	nni_mtx_init(&s->mtx);

#ifdef NNG_ENABLE_STATS
	static const nni_stat_info mixserver_info = {
		.si_name = "mixserver",
		.si_desc = "mixserveramorous mode?",
		.si_type = NNG_STAT_BOOLEAN,
	};
	static const nni_stat_info mismatch_info = {
		.si_name   = "mismatch",
		.si_desc   = "pipes rejected (protocol mismatch)",
		.si_type   = NNG_STAT_COUNTER,
		.si_atomic = true,
	};
	static const nni_stat_info already_info = {
		.si_name   = "already",
		.si_desc   = "pipes rejected (already connected)",
		.si_type   = NNG_STAT_COUNTER,
		.si_atomic = true,
	};
	static const nni_stat_info tx_drop_info = {
		.si_name   = "tx_drop",
		.si_desc   = "messages dropped undeliverable",
		.si_type   = NNG_STAT_COUNTER,
		.si_unit   = NNG_UNIT_MESSAGES,
		.si_atomic = true,
	};
	static const nni_stat_info rx_malformed_info = {
		.si_name   = "rx_malformed",
		.si_desc   = "malformed messages received",
		.si_type   = NNG_STAT_COUNTER,
		.si_unit   = NNG_UNIT_MESSAGES,
		.si_atomic = true,
	};
	static const nni_stat_info tx_malformed_info = {
		.si_name   = "tx_malformed",
		.si_desc   = "malformed messages not sent",
		.si_type   = NNG_STAT_COUNTER,
		.si_unit   = NNG_UNIT_MESSAGES,
		.si_atomic = true,
	};

	mixserver_add_sock_stat(s, &s->stat_mixserver, &mixserver_info);
	mixserver_add_sock_stat(s, &s->stat_reject_mismatch, &mismatch_info);
	mixserver_add_sock_stat(s, &s->stat_reject_already, &already_info);
	mixserver_add_sock_stat(s, &s->stat_tx_drop, &tx_drop_info);
	mixserver_add_sock_stat(s, &s->stat_rx_malformed, &rx_malformed_info);
	mixserver_add_sock_stat(s, &s->stat_tx_malformed, &tx_malformed_info);

	nni_stat_set_bool(&s->stat_mixserver, true);
#endif

	int rv;
	//give server more buffer
	if(((rv = nni_msgq_init(&s->urq_urgent,8)) != 0) || 
	((rv = nni_msgq_init(&s->urq_normal,8)) != 0) ||
	((rv = nni_msgq_init(&s->urq_unimportant,8)) != 0)
	){
		return rv;
	}
	
	/*
	nni_pollable *p;
	if ((rv = nni_msgq_get_recvable(s->urq_urgent,&p)) != 0){
		return rv;
	}else{
		if ((rv = nni_pollable_getfd(p,&s->fd_urgent)) != 0){
			return rv;
		}
	}
	if ((rv = nni_msgq_get_recvable(s->urq_normal,&p)) != 0){
		return rv;
	}else{
		if ((rv = nni_pollable_getfd(p,&s->fd_normal)) != 0){
			return rv;
		}
	}
	if ((rv = nni_msgq_get_recvable(s->urq_unimportant,&p)) != 0){
		return rv;
	}else{
		if ((rv = nni_pollable_getfd(p,&s->fd_unimportant)) != 0){
			return rv;
		}
	}*/
	return (0);
}

static void
mixserver_pipe_stop(void *arg)
{
	mixserver_pipe *p = arg;

	nni_aio_stop(&p->aio_send);
	nni_aio_stop(&p->aio_recv);
	nni_aio_stop(&p->aio_put);
	nni_aio_stop(&p->aio_get);
}

static void
mixserver_pipe_fini(void *arg)
{
	mixserver_pipe *p = arg;

	nni_aio_fini(&p->aio_send);
	nni_aio_fini(&p->aio_recv);
	nni_aio_fini(&p->aio_put);
	nni_aio_fini(&p->aio_get);
	nni_msgq_fini(p->send_queue);
}

static int
mixserver_pipe_init(void *arg, nni_pipe *pipe, void *pair)
{
	mixserver_pipe *p = arg;
	int             rv;

	nni_aio_init(&p->aio_send, mixserver_pipe_send_cb, p);
	nni_aio_init(&p->aio_recv, mixserver_pipe_recv_cb, p);
	nni_aio_init(&p->aio_get, mixserver_pipe_get_cb, p);
	nni_aio_init(&p->aio_put, mixserver_pipe_put_cb, p);

	if ((rv = nni_msgq_init(&p->send_queue, 1)) != 0) {
		mixserver_pipe_fini(p);
		return (rv);
	}

	/*
	nni_pollable* tmp;
	if ((rv = nni_msgq_get_sendable(p->send_queue,&tmp)) != 0){
		return rv;
	}else{
		if ((rv = nni_pollable_getfd(tmp,&p->fd_sendable)) != 0){
			return rv;
		}
	}*/

	p->pipe = pipe;
	p->pair = pair;

	return (0);
}

static int
mixserver_pipe_start(void *arg)
{
	mixserver_pipe *p = arg;
	mixserver_sock *s = p->pair;
	uint32_t        id;
	int             rv;

	nni_mtx_lock(&s->mtx);
	if (nni_pipe_peer(p->pipe) != NNG_MIXSERVER_PEER) {
		nni_mtx_unlock(&s->mtx);
		BUMP_STAT(&s->stat_reject_mismatch);
		// Peer protocol mismatch.
		return (NNG_EPROTO);
	}

	// add the new pipe in sock
	id = nni_pipe_id(p->pipe);
	if ((rv = nni_id_set(&s->pipes, id, p)) != 0) {
		nni_mtx_unlock(&s->mtx);
		return (rv);
	}

	nni_mtx_unlock(&s->mtx);

	// Schedule a get.  In mixserveramorous mode we get on the per pipe
	// send_queue, as the socket distributes to us. In monogamous mode
	// we bypass and get from the upper write queue directly (saving a
	// set of context switches).
	nni_msgq_aio_get(p->send_queue, &p->aio_get);

	// And the pipe read of course.
	nni_pipe_recv(p->pipe, &p->aio_recv);

	return (0);
}

static void
mixserver_pipe_close(void *arg)
{
	mixserver_pipe *p = arg;
	mixserver_sock *s = p->pair;

	nni_aio_close(&p->aio_send);
	nni_aio_close(&p->aio_recv);
	nni_aio_close(&p->aio_put);
	nni_aio_close(&p->aio_get);

	nni_mtx_lock(&s->mtx);
	nni_id_remove(&s->pipes, nni_pipe_id(p->pipe));
	nni_mtx_unlock(&s->mtx);

	nni_msgq_close(p->send_queue);
}

static void
mixserver_pipe_recv_cb(void *arg)
{
	mixserver_pipe *p = arg;
	mixserver_sock *s = p->pair;
	nni_msg *       msg;
	nni_pipe *      pipe = p->pipe;
	size_t          len;

	if (nni_aio_result(&p->aio_recv) != 0) {
		nni_pipe_close(p->pipe);
		return;
	}

	msg = nni_aio_get_msg(&p->aio_recv);
	nni_aio_set_msg(&p->aio_recv, NULL);

	// Store the pipe ID.
	nni_msg_set_pipe(msg, nni_pipe_id(p->pipe));

	if (nni_msg_len(msg) < sizeof(uint16_t)) {
		BUMP_STAT(&s->stat_rx_malformed);
		nni_msg_free(msg);
		nni_pipe_close(pipe);
		return;
	}
	uint8_t urgency_level = nni_msg_trim_u8(msg);
	uint8_t send_policy_code = nni_msg_trim_u8(msg);
	nni_msg_header_append_u8(msg,urgency_level);
	nni_msg_header_append_u8(msg,send_policy_code);

	switch(send_policy_code){
		case NNG_SENDPOLICY_RAW:{
			break;
		}
		case NNG_SENDPOLICY_SAMPLE:{
			//TODO
			break;
		}
		case NNG_SENDPOLICY_DEFAULT:{
			//TODO
			break;
		}
		//wrong code means err and we won't get the following content
		default:{
			BUMP_STAT(&s->stat_rx_malformed);
			nni_msg_free(msg);
			nni_pipe_close(pipe);
			return;
		}
	}
	len = nni_msg_len(msg);

	// Send the message up.
	nni_aio_set_msg(&p->aio_put, msg);
	nni_sock_bump_rx(s->sock, len);
	switch(urgency_level){
		case NNG_RECVPOLICY_URGENT:{
			nni_msgq_aio_put(s->urq_urgent, &p->aio_put);
			break;
		}
		case NNG_RECVPOLICY_UNIMPORTANT:{
			nni_msgq_aio_put(s->urq_unimportant, &p->aio_put);
			break;
		}
		// other msgs are all belong to normal
		default:{
			nni_msgq_aio_put(s->urq_normal, &p->aio_put);
		}
	}
}

static void
mixserver_pipe_put_cb(void *arg)
{
	mixserver_pipe *p = arg;

	if (nni_aio_result(&p->aio_put) != 0) {
		nni_msg_free(nni_aio_get_msg(&p->aio_put));
		nni_aio_set_msg(&p->aio_put, NULL);
		nni_pipe_close(p->pipe);
		return;
	}
	nni_pipe_recv(p->pipe, &p->aio_recv);
}

static void
mixserver_pipe_get_cb(void *arg)
{
	mixserver_pipe *p = arg;
	nni_msg *       msg;

	if (nni_aio_result(&p->aio_get) != 0) {
		nni_pipe_close(p->pipe);
		return;
	}

	msg = nni_aio_get_msg(&p->aio_get);
	nni_aio_set_msg(&p->aio_get, NULL);

	nni_aio_set_msg(&p->aio_send, msg);
	nni_pipe_send(p->pipe, &p->aio_send);
}

static void
mixserver_pipe_send_cb(void *arg)
{
	mixserver_pipe *p = arg;

	if (nni_aio_result(&p->aio_send) != 0) {
		nni_msg_free(nni_aio_get_msg(&p->aio_send));
		nni_aio_set_msg(&p->aio_send, NULL);
		nni_pipe_close(p->pipe);
		return;
	}

	nni_msgq_aio_get(p->send_queue, &p->aio_get);
}

static void
mixserver_sock_send(void *arg, nni_aio *aio)
{
	mixserver_sock *s = arg;

	nni_msg *msg= nni_aio_get_msg(aio);
	size_t len = nni_msg_len(msg);
	nni_sock_bump_tx(s->sock, len);

	if (nni_aio_begin(aio) != 0) {
		return;
	}

	uint32_t id = nni_msg_get_pipe(msg); 
	if(id != 0){
		//mixserver only use RAW mode and must specify a pipe_id
		//but the msg header contain the initial msg header from client
		//to help client know what this response is
		if(nni_msg_header_len(msg) < sizeof(uint16_t)){
			BUMP_STAT(&s->stat_tx_malformed);
			nni_aio_finish_error(aio,NNG_EINVAL);//NNG_EINVAL maybe not so clear
			return;
		}
		nni_mtx_lock(&s->mtx);
		mixserver_pipe *p = nni_id_get(&s->pipes,id);
		if((p == NULL) || nni_msgq_tryput(p->send_queue,msg) != 0){
			BUMP_STAT(&s->stat_tx_drop);
			nni_mtx_unlock(&s->mtx);
			nni_msgq_aio_put(p->send_queue,aio);
			return;
		}
	}else{
		nni_aio_finish_error(aio,NNG_EINVAL);//NNG_EINVAL maybe not so clear
		return;
	}
	nni_mtx_unlock(&s->mtx);
	nni_aio_set_msg(aio, NULL);
	nni_aio_finish(aio,0,len);
	return;
}

static void
mixserver_sock_recv(void *arg, nni_aio *aio)
{
	mixserver_sock *s = arg;
	nni_msg* msg;
	int rv = nni_msgq_tryget(s->urq_urgent,&msg);
	if(rv == NNG_ECLOSED){
		//any msgq closed means some error happens
		nni_aio_finish_error(aio,rv);
		return;
	}else if(rv == 0){
		nni_aio_finish_msg(aio, msg);
		return;
	}

	rv = nni_msgq_tryget(s->urq_normal,&msg);
	if(rv == NNG_ECLOSED){
		//any msgq closed means some error happens
		nni_aio_finish_error(aio,rv);
		return;
	}else if(rv == 0){
		nni_aio_finish_msg(aio, msg);
		return;
	}
	
	rv = nni_msgq_tryget(s->urq_unimportant,&msg);
	if(rv == NNG_ECLOSED){
		//any msgq closed means some error happens
		nni_aio_finish_error(aio,rv);
		return;
	}else if(rv == 0){
		nni_aio_finish_msg(aio, msg);
		return;
	}
	
	switch(s->recv_policy){
		case NNG_RECVPOLICY_NORMAL:{
			nni_msgq_aio_get(s->urq_normal,aio);
			break;
		}
		case NNG_RECVPOLICY_UNIMPORTANT:{
			nni_msgq_aio_get(s->urq_unimportant,aio);
			break;
		}
		default:{
			nni_msgq_aio_get(s->urq_urgent,aio);
			break;
		}
	}
	return;
}

static int
mixserver_set_recv_policy(void *arg, const void *buf, size_t sz, nni_opt_type t)
{
	mixserver_sock *s = arg;

	return (nni_copyin_int(&s->recv_policy, buf, sz, -1, NNG_RECVPOLICY_UNIMPORTANT, t));
}

static int
mixserver_get_recv_policy(void *arg, void *buf, size_t *szp, nni_opt_type t)
{
	mixserver_sock *s = arg;
	return (nni_copyout_int(s->recv_policy, buf, szp, t));
}

static nni_proto_pipe_ops mixserver_pipe_ops = {
	.pipe_size  = sizeof(mixserver_pipe),
	.pipe_init  = mixserver_pipe_init,
	.pipe_fini  = mixserver_pipe_fini,
	.pipe_start = mixserver_pipe_start,
	.pipe_close = mixserver_pipe_close,
	.pipe_stop  = mixserver_pipe_stop,
};

static nni_option mixserver_sock_options[] = {
	{
	    .o_name = NNG_OPT_MIX_RECVPOLICY,
	    .o_get  = mixserver_get_recv_policy,
	    .o_set  = mixserver_set_recv_policy,
	},
	// terminate list
	{
	    .o_name = NULL,
	},
};

static nni_proto_sock_ops mixserver_sock_ops = {
	.sock_size    = sizeof(mixserver_sock),
	.sock_init    = mixserver_sock_init,
	.sock_fini    = mixserver_sock_fini,
	.sock_open    = mixserver_sock_open,
	.sock_close   = mixserver_sock_close,
	.sock_recv    = mixserver_sock_recv,
	.sock_send    = mixserver_sock_send,
	.sock_options = mixserver_sock_options,
};

static nni_proto mixserver_proto = {
	.proto_version  = NNI_PROTOCOL_VERSION,
	.proto_self     = { NNG_MIXSERVER_SELF, NNG_MIXSERVER_SELF_NAME },
	.proto_peer     = { NNG_MIXSERVER_PEER, NNG_MIXSERVER_PEER_NAME },
	.proto_flags    = NNI_PROTO_FLAG_SNDRCV,
	.proto_sock_ops = &mixserver_sock_ops,
	.proto_pipe_ops = &mixserver_pipe_ops,
};

int
nng_mixserver_open(nng_socket *sock)
{
	return (nni_proto_open(sock, &mixserver_proto));
}
//#endif