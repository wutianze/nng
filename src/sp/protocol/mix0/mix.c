//
// Copyright 2020 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2018 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#include <stdlib.h>

#include "core/nng_impl.h"
#include "nng/protocol/mix/mix.h"

// Mix protocol provides the ability to deal with multi-interfaces scenario.
// Mix is like pair1_poly.

#ifdef NNG_ENABLE_STATS
#define BUMP_STAT(x) nni_stat_inc(x, 1)
#else
#define BUMP_STAT(x)
#endif

typedef struct mix_pipe mix_pipe;
typedef struct mix_sock mix_sock;

static void mix_sock_get_cb(void *);
static void mix_pipe_send_cb(void *);
static void mix_pipe_recv_cb(void *);
static void mix_pipe_get_cb(void *);
static void mix_pipe_put_cb(void *);
static void mix_pipe_fini(void *);

/*use func pointer will bring sync overhead
struct mix_send_policy_ops{
	int (*choose_and_send)(mix_sock*, nni_msg*);
};
struct mix_recv_policy_ops{
	int (*recv_and_choose)(mix_sock*, nni_msg*);
};

typedef struct mix_send_policy_ops mix_send_policy_ops;
typedef struct mix_recv_policy_ops mix_recv_policy_ops;
*/ 
// mix_sock is our per-socket protocol private structure.
struct mix_sock {
	nni_sock      *sock;
	nni_msgq      *uwq;
	nni_msgq      *urq_urgent;
	nni_msgq      *urq_normal;
	nni_msgq      *urq_unimportant;
	nni_mtx        mtx;
	nni_id_map     pipes;
	nni_list       delay_list;
	nni_list       bw_list;
	nni_list       reliable_list;
	nni_list       safe_list;
	bool           started;
	nni_aio        aio_get;

	//policy related
	int            recv_policy;
	int            send_policy;
#ifdef NNG_ENABLE_STATS
	nni_stat_item  stat_mix;
	nni_stat_item  stat_reject_mismatch;
	nni_stat_item  stat_reject_already;
	nni_stat_item  stat_rx_malformed;
	nni_stat_item  stat_tx_malformed;
	nni_stat_item  stat_tx_drop;
#endif
};

// mix_pipe is our per-pipe protocol private structure.
struct mix_pipe {
	nni_pipe       *pipe;
	mix_sock       *pair;
	nni_msgq       *send_queue;
	nni_aio         aio_send;
	nni_aio         aio_recv;
	nni_aio         aio_get;
	nni_aio         aio_put;
	nni_list_node   node_delay;
	nni_list_node   node_bw;
	nni_list_node   node_reliable;
	nni_list_node   node_safe;
};

static void
mix_sock_fini(void *arg)
{
	mix_sock *s = arg;

	nni_aio_fini(&s->aio_get);
	nni_id_map_fini(&s->pipes);
	nni_msgq_fini(s->urq_urgent);
	nni_msgq_fini(s->urq_normal);
	nni_msgq_fini(s->urq_unimportant);
	nni_mtx_fini(&s->mtx);
}

#ifdef NNG_ENABLE_STATS
static void
mix_add_sock_stat(
    mix_sock *s, nni_stat_item *item, const nni_stat_info *info)
{
	nni_stat_init(item, info);
	nni_sock_add_stat(s->sock, item);
}
#endif

static void
mix_sock_open(void *arg)
{
	NNI_ARG_UNUSED(arg);
}

static void
mix_sock_close(void *arg)
{
	mix_sock *s = arg;
	nni_aio_close(&s->aio_get);
	nni_msgq_close(s->urq_urgent);
	nni_msgq_close(s->urq_normal);
	nni_msgq_close(s->urq_unimportant);
}

static int
mix_sock_init(void *arg, nni_sock *sock)
{
	mix_sock *s = arg;

	nni_id_map_init(&s->pipes, 0, 0, false);
	NNI_LIST_INIT(&s->delay_list, mix_pipe, node_delay);
	NNI_LIST_INIT(&s->bw_list, mix_pipe, node_bw);
	NNI_LIST_INIT(&s->reliable_list, mix_pipe, node_reliable);
	NNI_LIST_INIT(&s->safe_list, mix_pipe, node_safe);
	s->sock = sock;

	// Raw mode uses this.
	nni_mtx_init(&s->mtx);

	nni_aio_init(&s->aio_get, mix_sock_get_cb, s);

#ifdef NNG_ENABLE_STATS
	static const nni_stat_info mix_info = {
		.si_name = "mix",
		.si_desc = "mixamorous mode?",
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

	mix_add_sock_stat(s, &s->stat_mix, &mix_info);
	mix_add_sock_stat(s, &s->stat_reject_mismatch, &mismatch_info);
	mix_add_sock_stat(s, &s->stat_reject_already, &already_info);
	mix_add_sock_stat(s, &s->stat_tx_drop, &tx_drop_info);
	mix_add_sock_stat(s, &s->stat_rx_malformed, &rx_malformed_info);
	mix_add_sock_stat(s, &s->stat_tx_malformed, &tx_malformed_info);

	nni_stat_set_bool(&s->stat_mix, true);
#endif

	int rv;
	s->uwq = nni_sock_sendq(sock);
	if(((rv = nni_msgq_init(&s->urq_urgent,2)) != 0) || 
	((rv = nni_msgq_init(&s->urq_normal,2)) != 0) ||
	((rv = nni_msgq_init(&s->urq_unimportant,2)) != 0)
	){
		return rv;
	}

	return (0);
}

static void
mix_pipe_stop(void *arg)
{
	mix_pipe *p = arg;

	nni_aio_stop(&p->aio_send);
	nni_aio_stop(&p->aio_recv);
	nni_aio_stop(&p->aio_put);
	nni_aio_stop(&p->aio_get);
}

static void
mix_pipe_fini(void *arg)
{
	mix_pipe *p = arg;

	nni_aio_fini(&p->aio_send);
	nni_aio_fini(&p->aio_recv);
	nni_aio_fini(&p->aio_put);
	nni_aio_fini(&p->aio_get);
	nni_msgq_fini(p->send_queue);
}

static int
mix_pipe_init(void *arg, nni_pipe *pipe, void *pair)
{
	mix_pipe *p = arg;
	int             rv;

	nni_aio_init(&p->aio_send, mix_pipe_send_cb, p);
	nni_aio_init(&p->aio_recv, mix_pipe_recv_cb, p);
	nni_aio_init(&p->aio_get, mix_pipe_get_cb, p);
	nni_aio_init(&p->aio_put, mix_pipe_put_cb, p);

	if ((rv = nni_msgq_init(&p->send_queue, 2)) != 0) {
		mix_pipe_fini(p);
		return (rv);
	}

	p->pipe = pipe;
	p->pair = pair;

	return (0);
}

static int
insert_in_pipe_list(mix_pipe*new_pipe, nni_list*list_pipe, const char*which){
	int rv = 0;
	mix_pipe*exist_pipe;
	int new_val = 0;
	if((rv = nni_pipe_getopt(new_pipe->pipe, which,&new_val,NULL,NNI_TYPE_UINT32))!=0){
		return rv;
	}
	NNI_LIST_FOREACH(list_pipe,exist_pipe){
		int old_val = 0;
		if((rv = nni_pipe_getopt(exist_pipe->pipe, which,&old_val,NULL,NNI_TYPE_UINT32))!=0){
			return rv;
		}
		if(old_val <= new_val){// new_pipe is better
			nni_list_insert_before(list_pipe,new_pipe,exist_pipe);
			return 0;
		}
	}
}

static int
mix_pipe_start(void *arg)
{
	mix_pipe *p = arg;
	mix_sock *s = p->pair;
	uint32_t        id;
	int             rv;

	nni_mtx_lock(&s->mtx);
	if (nni_pipe_peer(p->pipe) != NNG_MIX_PEER) {
		nni_mtx_unlock(&s->mtx);
		BUMP_STAT(&s->stat_reject_mismatch);
		// Peer protocol mismatch.
		return (NNG_EPROTO);
	}

	id = nni_pipe_id(p->pipe);
	if ((rv = nni_id_set(&s->pipes, id, p)) != 0) {
		nni_mtx_unlock(&s->mtx);
		return (rv);
	}
	if (!s->started) {
		nni_msgq_aio_get(s->uwq, &s->aio_get);
	}
	nni_list_append(&s->plist, p);
	s->started = true;
	nni_mtx_unlock(&s->mtx);

	// Schedule a get.  In mixamorous mode we get on the per pipe
	// send_queue, as the socket distributes to us. In monogamous mode
	// we bypass and get from the upper write queue directly (saving a
	// set of context switches).
	nni_msgq_aio_get(p->send_queue, &p->aio_get);

	// And the pipe read of course.
	nni_pipe_recv(p->pipe, &p->aio_recv);

	return (0);
}

static void
mix_pipe_close(void *arg)
{
	mix_pipe *p = arg;
	mix_sock *s = p->pair;

	nni_aio_close(&p->aio_send);
	nni_aio_close(&p->aio_recv);
	nni_aio_close(&p->aio_put);
	nni_aio_close(&p->aio_get);

	nni_mtx_lock(&s->mtx);
	nni_id_remove(&s->pipes, nni_pipe_id(p->pipe));
	nni_list_node_remove(&p->node);
	nni_mtx_unlock(&s->mtx);

	nni_msgq_close(p->send_queue);
}

static void
mix_pipe_recv_cb(void *arg)
{
	mix_pipe *p = arg;
	mix_sock *s = p->pair;
	nni_msg *       msg;
	uint32_t        hdr;
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

	// If the message is missing the hop count header, scrap it.
	if ((nni_msg_len(msg) < sizeof(uint32_t)) ||
	    ((hdr = nni_msg_trim_u32(msg)) > 0xff)) {
		BUMP_STAT(&s->stat_rx_malformed);
		nni_msg_free(msg);
		nni_pipe_close(pipe);
		return;
	}

	len = nni_msg_len(msg);

	// If we bounced too many times, discard the message, but
	// keep getting more.
	if ((int) hdr > nni_atomic_get(&s->ttl)) {
		BUMP_STAT(&s->stat_ttl_drop);
		nni_msg_free(msg);
		nni_pipe_recv(pipe, &p->aio_recv);
		return;
	}

	// Store the hop count in the header.
	nni_msg_header_append_u32(msg, hdr);

	// Send the message up.
	nni_aio_set_msg(&p->aio_put, msg);
	nni_sock_bump_rx(s->sock, len);
	nni_msgq_aio_put(s->urq, &p->aio_put);
}

static void
mix_sock_get_cb(void *arg)
{
	mix_pipe *p;
	mix_sock *s = arg;
	nni_msg *       msg;
	uint32_t        id;

	if (nni_aio_result(&s->aio_get) != 0) {
		// Socket closing...
		return;
	}

	msg = nni_aio_get_msg(&s->aio_get);
	nni_aio_set_msg(&s->aio_get, NULL);

	p = NULL;
	nni_mtx_lock(&s->mtx);
	// If no pipe was requested, we look for any connected peer.
	if (((id = nni_msg_get_pipe(msg)) == 0) &&
	    (!nni_list_empty(&s->plist))) {
		p = nni_list_first(&s->plist);
	} else {
		p = nni_id_get(&s->pipes, id);
	}

	// Try a non-blocking send.  If this fails we just discard the
	// message.  We have to do this to avoid head-of-line blocking
	// for messages sent to other pipes.  Note that there is some
	// buffering in the send_queue.
	if ((p == NULL) || nni_msgq_tryput(p->send_queue, msg) != 0) {
		BUMP_STAT(&s->stat_tx_drop);
		nni_msg_free(msg);
	}

	nni_mtx_unlock(&s->mtx);
	nni_msgq_aio_get(s->uwq, &s->aio_get);
}

static void
mix_pipe_put_cb(void *arg)
{
	mix_pipe *p = arg;

	if (nni_aio_result(&p->aio_put) != 0) {
		nni_msg_free(nni_aio_get_msg(&p->aio_put));
		nni_aio_set_msg(&p->aio_put, NULL);
		nni_pipe_close(p->pipe);
		return;
	}
	nni_pipe_recv(p->pipe, &p->aio_recv);
}

static void
mix_pipe_get_cb(void *arg)
{
	mix_pipe *p = arg;
	nni_msg *       msg;

	if (nni_aio_result(&p->aio_get) != 0) {
		nni_pipe_close(p->pipe);
		return;
	}

	msg = nni_aio_get_msg(&p->aio_get);
	nni_aio_set_msg(&p->aio_get, NULL);

	// Cooked mode messages have no header so we have to add one.
	// Strip off any previously existing header, such as when
	// replying to messages.
	nni_msg_header_clear(msg);

	// Insert the hops header.
	nni_msg_header_append_u32(msg, 1);

	nni_aio_set_msg(&p->aio_send, msg);
	nni_pipe_send(p->pipe, &p->aio_send);
}

static void
mix_pipe_send_cb(void *arg)
{
	mix_pipe *p = arg;

	if (nni_aio_result(&p->aio_send) != 0) {
		nni_msg_free(nni_aio_get_msg(&p->aio_send));
		nni_aio_set_msg(&p->aio_send, NULL);
		nni_pipe_close(p->pipe);
		return;
	}

	nni_msgq_aio_get(p->send_queue, &p->aio_get);
}



static int
mix_set_send_policy(void *arg, const void *buf, size_t sz, nni_opt_type t)
{
	mix_sock *s = arg;

	return (nni_copyin_int(&s->send_policy, buf, sz, -1, NNI_SENDPOLICY_DEFAULT, t));
}

static int
mix_get_send_policy(void *arg, void *buf, size_t *szp, nni_opt_type t)
{
	mix_sock *s = arg;
	return (nni_copyout_int(s->send_policy, buf, szp, t));
}

static int
mix_set_recv_policy(void *arg, const void *buf, size_t sz, nni_opt_type t)
{
	mix_sock *s = arg;

	return (nni_copyin_int(&s->recv_policy, buf, sz, -1, NNI_RECVPOLICY_UNIMPORTANT, t));
}

static int
mix_get_recv_policy(void *arg, void *buf, size_t *szp, nni_opt_type t)
{
	mix_sock *s = arg;
	return (nni_copyout_int(s->recv_policy, buf, szp, t));
}

static void
mix_sock_send(void *arg, nni_aio *aio)
{
	mix_sock *s = arg;

	nni_sock_bump_tx(s->sock, nni_msg_len(nni_aio_get_msg(aio)));
	nni_msgq_aio_put(s->uwq, aio);
}

static void
mix_sock_recv(void *arg, nni_aio *aio)
{
	mix_sock *s = arg;

	nni_msgq_aio_get(s->urq, aio);
}

static nni_proto_pipe_ops mix_pipe_ops = {
	.pipe_size  = sizeof(mix_pipe),
	.pipe_init  = mix_pipe_init,
	.pipe_fini  = mix_pipe_fini,
	.pipe_start = mix_pipe_start,
	.pipe_close = mix_pipe_close,
	.pipe_stop  = mix_pipe_stop,
};

static nni_option mix_sock_options[] = {
	{
	    .o_name = NNG_OPT_SENDPOLICY,
	    .o_get  = mix_get_send_policy,
	    .o_set  = mix_set_send_policy,
	},
	{
	    .o_name = NNG_OPT_RECVPOLICY,
	    .o_get  = mix_get_recv_policy,
	    .o_set  = mix_set_recv_policy,
	},
	// terminate list
	{
	    .o_name = NULL,
	},
};

static nni_proto_sock_ops mix_sock_ops = {
	.sock_size    = sizeof(mix_sock),
	.sock_init    = mix_sock_init,
	.sock_fini    = mix_sock_fini,
	.sock_open    = mix_sock_open,
	.sock_close   = mix_sock_close,
	.sock_recv    = mix_sock_recv,
	.sock_send    = mix_sock_send,
	.sock_options = mix_sock_options,
};

static nni_proto mix_proto = {
	.proto_version  = NNI_PROTOCOL_VERSION,
	.proto_self     = { NNG_MIX_SELF, NNG_MIX_SELF_NAME },
	.proto_peer     = { NNG_MIX_PEER, NNG_MIX_PEER_NAME },
	.proto_flags    = NNI_PROTO_FLAG_SNDRCV,
	.proto_sock_ops = &mix_sock_ops,
	.proto_pipe_ops = &mix_pipe_ops,
};

int
nng_mix_open(nng_socket *sock)
{
	return (nni_proto_open(sock, &mix_proto));
}
