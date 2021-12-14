//
// Copyright 2020 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2018 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

// now we do not support windows platform in mixclient
//#ifndef _WIN32

#include <stdlib.h>

#include "core/nng_impl.h"
#include "nng/protocol/mix0/mixclient.h"

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

typedef struct mixclient_pipe mixclient_pipe;
typedef struct mixclient_sock mixclient_sock;

static void mixclient_sock_get_cb(void *);
static void mixclient_pipe_send_cb(void *);
static void mixclient_pipe_recv_cb(void *);
static void mixclient_pipe_get_cb(void *);
static void mixclient_pipe_put_cb(void *);
static void mixclient_pipe_fini(void *);

/*use func pointer will bring sync overhead
struct mixclient_send_policy_ops{
	int (*choose_and_send)(mixclient_sock*, nni_msg*);
};
struct mixclient_recv_policy_ops{
	int (*recv_and_choose)(mixclient_sock*, nni_msg*);
};

typedef struct mixclient_send_policy_ops mixclient_send_policy_ops;
typedef struct mixclient_recv_policy_ops mixclient_recv_policy_ops;
*/ 
// mixclient_sock is our per-socket protocol private structure.
struct mixclient_sock {
	nni_sock      *sock;
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
	nni_list       delay_list;
	nni_list       bw_list;
	nni_list       reliable_list;
	nni_list       safe_list;

	//policy related
	int            recv_policy;
	int            send_policy;
#ifdef NNG_ENABLE_STATS
	nni_stat_item  stat_mixclient;
	nni_stat_item  stat_reject_mismatch;
	nni_stat_item  stat_reject_already;
	nni_stat_item  stat_rx_malformed;
	nni_stat_item  stat_tx_malformed;
	nni_stat_item  stat_tx_drop;
#endif
};

// mixclient_pipe is our per-pipe protocol private structure.
struct mixclient_pipe {
	nni_pipe       *pipe;
	mixclient_sock       *pair;
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
mixclient_sock_fini(void *arg)
{
	mixclient_sock *s = arg;

	nni_id_map_fini(&s->pipes);
	nni_msgq_fini(s->urq_urgent);
	nni_msgq_fini(s->urq_normal);
	nni_msgq_fini(s->urq_unimportant);
	nni_mtx_fini(&s->mtx);
}

#ifdef NNG_ENABLE_STATS
static void
mixclient_add_sock_stat(
    mixclient_sock *s, nni_stat_item *item, const nni_stat_info *info)
{
	nni_stat_init(item, info);
	nni_sock_add_stat(s->sock, item);
}
#endif

static void
mixclient_sock_open(void *arg)
{
	NNI_ARG_UNUSED(arg);
}

static void
mixclient_sock_close(void *arg)
{
	mixclient_sock *s = arg;
	nni_msgq_close(s->urq_urgent);
	nni_msgq_close(s->urq_normal);
	nni_msgq_close(s->urq_unimportant);
}

static int
mixclient_sock_init(void *arg, nni_sock *sock)
{
	mixclient_sock *s = arg;

	nni_id_map_init(&s->pipes, 0, 0, false);
	NNI_LIST_INIT(&s->delay_list, mixclient_pipe, node_delay);
	NNI_LIST_INIT(&s->bw_list, mixclient_pipe, node_bw);
	NNI_LIST_INIT(&s->reliable_list, mixclient_pipe, node_reliable);
	NNI_LIST_INIT(&s->safe_list, mixclient_pipe, node_safe);
	s->sock = sock;

	// Raw mode uses this.
	nni_mtx_init(&s->mtx);

#ifdef NNG_ENABLE_STATS
	static const nni_stat_info mixclient_info = {
		.si_name = "mixclient",
		.si_desc = "mixclientamorous mode?",
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

	mixclient_add_sock_stat(s, &s->stat_mixclient, &mixclient_info);
	mixclient_add_sock_stat(s, &s->stat_reject_mismatch, &mismatch_info);
	mixclient_add_sock_stat(s, &s->stat_reject_already, &already_info);
	mixclient_add_sock_stat(s, &s->stat_tx_drop, &tx_drop_info);
	mixclient_add_sock_stat(s, &s->stat_rx_malformed, &rx_malformed_info);
	mixclient_add_sock_stat(s, &s->stat_tx_malformed, &tx_malformed_info);

	nni_stat_set_bool(&s->stat_mixclient, true);
#endif

	int rv;
	if(((rv = nni_msgq_init(&s->urq_urgent,2)) != 0) || 
	((rv = nni_msgq_init(&s->urq_normal,2)) != 0) ||
	((rv = nni_msgq_init(&s->urq_unimportant,2)) != 0)
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
mixclient_pipe_stop(void *arg)
{
	mixclient_pipe *p = arg;

	nni_aio_stop(&p->aio_send);
	nni_aio_stop(&p->aio_recv);
	nni_aio_stop(&p->aio_put);
	nni_aio_stop(&p->aio_get);
}

static void
mixclient_pipe_fini(void *arg)
{
	mixclient_pipe *p = arg;

	nni_aio_fini(&p->aio_send);
	nni_aio_fini(&p->aio_recv);
	nni_aio_fini(&p->aio_put);
	nni_aio_fini(&p->aio_get);
	nni_msgq_fini(p->send_queue);
}

static int
mixclient_pipe_init(void *arg, nni_pipe *pipe, void *pair)
{
	mixclient_pipe *p = arg;
	int             rv;

	nni_aio_init(&p->aio_send, mixclient_pipe_send_cb, p);
	nni_aio_init(&p->aio_recv, mixclient_pipe_recv_cb, p);
	nni_aio_init(&p->aio_get, mixclient_pipe_get_cb, p);
	nni_aio_init(&p->aio_put, mixclient_pipe_put_cb, p);

	if ((rv = nni_msgq_init(&p->send_queue, 1)) != 0) {
		mixclient_pipe_fini(p);
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
insert_in_pipe_list(nni_list*list_pipe, mixclient_pipe*new_pipe, const char*which){
	int rv = 0;
	mixclient_pipe*exist_pipe;
	int new_val = 0;
	if((rv = nni_pipe_getopt(new_pipe->pipe, which,&new_val,NULL,NNI_TYPE_INT32))!=0){
		return rv;
	}
	NNI_LIST_FOREACH(list_pipe,exist_pipe){
		int old_val = 0;
		if((rv = nni_pipe_getopt(exist_pipe->pipe, which,&old_val,NULL,NNI_TYPE_INT32))!=0){
			return rv;
		}
		if(old_val <= new_val){// new_pipe is better
			nni_list_insert_before(list_pipe,new_pipe,exist_pipe);
			return 0;
		}
	}
	nni_list_append(list_pipe,new_pipe);
	return 0;
}

static int
mixclient_pipe_start(void *arg)
{
	mixclient_pipe *p = arg;
	mixclient_sock *s = p->pair;
	uint32_t        id;
	int             rv;

	nni_mtx_lock(&s->mtx);
	if (nni_pipe_peer(p->pipe) != NNG_MIXCLIENT_PEER) {
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
	if ((rv = insert_in_pipe_list(&s->delay_list,p,NNG_OPT_INTERFACE_DELAY))!=0){
		nni_mtx_unlock(&s->mtx);
		return rv;
	}
	if ((rv = insert_in_pipe_list(&s->bw_list,p,NNG_OPT_INTERFACE_BW))!=0){
		nni_mtx_unlock(&s->mtx);
		return rv;
	}
	if ((rv = insert_in_pipe_list(&s->reliable_list,p,NNG_OPT_INTERFACE_RELIABLE))!=0){
		nni_mtx_unlock(&s->mtx);
		return rv;
	}
	if ((rv = insert_in_pipe_list(&s->safe_list,p,NNG_OPT_INTERFACE_SAFE))!=0){
		nni_mtx_unlock(&s->mtx);
		return rv;
	}

	nni_mtx_unlock(&s->mtx);

	// Schedule a get.  In mixclientamorous mode we get on the per pipe
	// send_queue, as the socket distributes to us. In monogamous mode
	// we bypass and get from the upper write queue directly (saving a
	// set of context switches).
	nni_msgq_aio_get(p->send_queue, &p->aio_get);

	// And the pipe read of course.
	nni_pipe_recv(p->pipe, &p->aio_recv);

	return (0);
}

static void
mixclient_pipe_close(void *arg)
{
	mixclient_pipe *p = arg;
	mixclient_sock *s = p->pair;

	nni_aio_close(&p->aio_send);
	nni_aio_close(&p->aio_recv);
	nni_aio_close(&p->aio_put);
	nni_aio_close(&p->aio_get);

	nni_mtx_lock(&s->mtx);
	nni_id_remove(&s->pipes, nni_pipe_id(p->pipe));
	nni_list_node_remove(&p->node_delay);
	nni_list_node_remove(&p->node_bw);
	nni_list_node_remove(&p->node_reliable);
	nni_list_node_remove(&p->node_safe);
	nni_mtx_unlock(&s->mtx);

	nni_msgq_close(p->send_queue);
}

static void
mixclient_pipe_recv_cb(void *arg)
{
	mixclient_pipe *p = arg;
	mixclient_sock *s = p->pair;
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

	// msg header from mixserver has two uint8
	if (nni_msg_len(msg) < sizeof(uint16_t)) {
		BUMP_STAT(&s->stat_rx_malformed);
		nni_msg_free(msg);
		nni_pipe_close(pipe);
		return;
	}
	uint8_t urgency_level = nni_msg_trim_u8(msg);
	uint8_t send_policy_code = nni_msg_trim_u8(msg);
	//move headers to their place
	nni_msg_header_append_u8(msg,urgency_level);
	nni_msg_header_append_u8(msg,send_policy_code);

	// we check if more val need to be moved	
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
			nni_pipe_close(pipe);// recv more or just close?
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

static int
tryput_in_pipe_list(nni_list*list_pipe, nni_msg*msg){
	int rv = 0;
	mixclient_pipe*exist_pipe;
	NNI_LIST_FOREACH(list_pipe,exist_pipe){
		if((rv = nni_msgq_tryput(exist_pipe->send_queue,msg))==0){
			return 0;
		}else{
			return rv;
		}
	}
	return NNG_EAGAIN;
}

// return the first pipe in the chosen_list
static mixclient_pipe*
choose_nature_list(uint8_t nature, mixclient_sock*s,nni_list**chosen_list){
	switch(nature){
			case NNG_MSG_INTERFACE_DELAY:
			*chosen_list = &s->delay_list;
			break;
			case NNG_MSG_INTERFACE_BW:
			*chosen_list = &s->bw_list;
			break;
			case NNG_MSG_INTERFACE_RELIABLE:
			*chosen_list = &s->reliable_list;
			break;
			//Other msgs use the safest pipe
			default:	
			*chosen_list = &s->safe_list;
	}
	if(!nni_list_empty(*chosen_list)){
		return nni_list_first(*chosen_list);
	}
	return NULL;
}

static void
mixclient_pipe_put_cb(void *arg)
{
	mixclient_pipe *p = arg;

	if (nni_aio_result(&p->aio_put) != 0) {
		nni_msg_free(nni_aio_get_msg(&p->aio_put));
		nni_aio_set_msg(&p->aio_put, NULL);
		nni_pipe_close(p->pipe);
		return;
	}
	nni_pipe_recv(p->pipe, &p->aio_recv);
}

static void
mixclient_pipe_get_cb(void *arg)
{
	mixclient_pipe *p = arg;
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
mixclient_pipe_send_cb(void *arg)
{
	mixclient_pipe *p = arg;

	if (nni_aio_result(&p->aio_send) != 0) {
		nni_msg_free(nni_aio_get_msg(&p->aio_send));
		nni_aio_set_msg(&p->aio_send, NULL);
		nni_pipe_close(p->pipe);
		return;
	}

	nni_msgq_aio_get(p->send_queue, &p->aio_get);
}

static void
mixclient_sock_send(void *arg, nni_aio *aio)
{
	mixclient_sock *s = arg;

	nni_msg *msg= nni_aio_get_msg(aio);
	size_t len = nni_msg_len(msg);
	nni_sock_bump_tx(s->sock, len);

	if (nni_aio_begin(aio) != 0) {
		return;
	}
	mixclient_pipe *p;
	switch(s->send_policy){
	case NNG_SENDPOLICY_RAW:{
		if(nni_msg_header_len(msg) != sizeof(uint16_t)){
			BUMP_STAT(&s->stat_tx_malformed);
			goto send_fail;
		}
		//uint8_t urgency_level = nni_msg_header_peek_u8(msg);
		uint8_t nature_chosen = nni_msg_header_chop_u8(msg);
		if(nni_msg_header_append_u8(msg, NNG_SENDPOLICY_RAW) != 0){
			goto send_fail;
		}
		uint32_t id = nni_msg_get_pipe(msg); 
		if(id != 0){
			nni_mtx_lock(&s->mtx);
			p = nni_id_get(&s->pipes,id);
			if((p == NULL) || nni_msgq_tryput(p->send_queue,msg) != 0){
				BUMP_STAT(&s->stat_tx_drop);
				goto wait_in_queue;
			}
		}else{
			nni_mtx_lock(&s->mtx);
			// maybe we should just use aio_put instead of trying every
			// pipe in the list?
			nni_list* chosen_list = NULL;
			p = choose_nature_list(nature_chosen,s,&chosen_list);
			if(tryput_in_pipe_list(chosen_list,msg) != 0){
				BUMP_STAT(&s->stat_tx_drop);
				goto wait_in_queue;
			}
		}
		break;
	}
	case NNG_SENDPOLICY_SAMPLE:{
		break;
	}
	case NNG_SENDPOLICY_DEFAULT:
	default:{
		break;
	}
	}
	nni_mtx_unlock(&s->mtx);
	nni_aio_set_msg(aio, NULL);
	nni_aio_finish(aio,0,len);
	return;
send_fail:
	//if fail, we dont get the msg's ownership
	nni_mtx_unlock(&s->mtx);
	nni_aio_finish_error(aio,NNG_EINVAL);//NNG_EINVAL maybe not so clear
	return;
wait_in_queue:
	nni_mtx_unlock(&s->mtx);
	nni_msgq_aio_put(p->send_queue,aio);
	return;
}

static void
mixclient_sock_recv(void *arg, nni_aio *aio)
{
	mixclient_sock *s = arg;
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

	//wait in the specified msgq
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
mixclient_set_send_policy(void *arg, const void *buf, size_t sz, nni_opt_type t)
{
	mixclient_sock *s = arg;

	return (nni_copyin_int(&s->send_policy, buf, sz, -1, NNG_SENDPOLICY_DEFAULT, t));
}

static int
mixclient_get_send_policy(void *arg, void *buf, size_t *szp, nni_opt_type t)
{
	mixclient_sock *s = arg;
	return (nni_copyout_int(s->send_policy, buf, szp, t));
}

static int
mixclient_set_recv_policy(void *arg, const void *buf, size_t sz, nni_opt_type t)
{
	mixclient_sock *s = arg;

	return (nni_copyin_int(&s->recv_policy, buf, sz, -1, NNG_RECVPOLICY_UNIMPORTANT, t));
}

static int
mixclient_get_recv_policy(void *arg, void *buf, size_t *szp, nni_opt_type t)
{
	mixclient_sock *s = arg;
	return (nni_copyout_int(s->recv_policy, buf, szp, t));
}

static nni_proto_pipe_ops mixclient_pipe_ops = {
	.pipe_size  = sizeof(mixclient_pipe),
	.pipe_init  = mixclient_pipe_init,
	.pipe_fini  = mixclient_pipe_fini,
	.pipe_start = mixclient_pipe_start,
	.pipe_close = mixclient_pipe_close,
	.pipe_stop  = mixclient_pipe_stop,
};

static nni_option mixclient_sock_options[] = {
	{
	    .o_name = NNG_OPT_MIX_SENDPOLICY,
	    .o_get  = mixclient_get_send_policy,
	    .o_set  = mixclient_set_send_policy,
	},
	{
	    .o_name = NNG_OPT_MIX_RECVPOLICY,
	    .o_get  = mixclient_get_recv_policy,
	    .o_set  = mixclient_set_recv_policy,
	},
	// terminate list
	{
	    .o_name = NULL,
	},
};

static nni_proto_sock_ops mixclient_sock_ops = {
	.sock_size    = sizeof(mixclient_sock),
	.sock_init    = mixclient_sock_init,
	.sock_fini    = mixclient_sock_fini,
	.sock_open    = mixclient_sock_open,
	.sock_close   = mixclient_sock_close,
	.sock_recv    = mixclient_sock_recv,
	.sock_send    = mixclient_sock_send,
	.sock_options = mixclient_sock_options,
};

static nni_proto mixclient_proto = {
	.proto_version  = NNI_PROTOCOL_VERSION,
	.proto_self     = { NNG_MIXCLIENT_SELF, NNG_MIXCLIENT_SELF_NAME },
	.proto_peer     = { NNG_MIXCLIENT_PEER, NNG_MIXCLIENT_PEER_NAME },
	.proto_flags    = NNI_PROTO_FLAG_SNDRCV,
	.proto_sock_ops = &mixclient_sock_ops,
	.proto_pipe_ops = &mixclient_pipe_ops,
};

int
nng_mixclient_open(nng_socket *sock)
{
	return (nni_proto_open(sock, &mixclient_proto));
}
//#endif
