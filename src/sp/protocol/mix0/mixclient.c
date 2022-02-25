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

static void mixclient_pipe_send_cb(void *);
static void mixclient_pipe_recv_cb(void *);
static void mixclient_pipe_put_cb(void *);
static void mixclient_pipe_fini(void *);
static void mixclient_pipe_send(mixclient_pipe *,nni_msg *);
static void mixclient_send_sched(mixclient_pipe *);

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

	nni_mtx       mtx_urq;
	nni_lmq       urq;//release this in sock close with mtx_urq, and lmq_fini in sock_fini will release this again without mtx_urq
	nni_list      raq;
	nni_list      paq;//aio_put from pipes
	nni_time      current_time;//useful for some policies like double

	/* replaced by adding tryget in msgqueue.c
	int            fd_urgent;
	int            fd_normal;
	int            fd_unimportant;
	*/

	// mtx is to protect id_map and list for pipes
	// for pipe close&start will make changes to them
	nni_mtx        mtx;	
	nni_id_map     pipes;
	nni_list       plist;

	//policy related
	//int            recv_policy;
	//int            send_policy;
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
	
	nni_mtx         mtx_send;
	nni_lmq         send_queue;//release this in pipe_stop and pipe_fini
	nni_list        waq;
	
	nni_aio         aio_send;
	nni_aio         aio_recv;
	
	nni_aio         aio_put;
	
	bool            w_ready;

	//may need a mtx later
	uint8_t delay;
	uint8_t bw;
	uint8_t reliable;
	uint8_t security;

	nni_list_node   node;
};

static void
mixclient_sock_fini(void *arg)
{
	mixclient_sock *s = arg;

	nni_id_map_fini(&s->pipes);
	nni_lmq_fini(&s->urq);
	nni_mtx_fini(&s->mtx_urq);
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
	nni_aio *a;
	nni_msg *m;
	nni_mtx_lock(&s->mtx_urq);
	while((a=nni_list_first(&s->raq))!=NULL ||((a=nni_list_first(&s->paq))!=NULL)){
		nni_aio_list_remove(a);
		nni_aio_finish_error(a,NNG_ECLOSED);
	}
	while(nni_lmq_get(&s->urq,&m) ==0){
		nni_msg_free(m);
	}
	nni_mtx_unlock(&s->mtx_urq);
}

static void
mixclient_sock_init(void *arg, nni_sock *sock)
{
	mixclient_sock *s = arg;

	nni_mtx_init(&s->mtx);
	nni_mtx_init(&s->mtx_urq);

	nni_id_map_init(&s->pipes, 0, 0, false);
	NNI_LIST_INIT(&s->plist, mixclient_pipe, node);
	nni_lmq_init(&s->urq,4);//should be big enough
	nni_aio_list_init(&s->raq);
	nni_aio_list_init(&s->paq);
	s->current_time = 0;
	s->sock = sock;


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
}

static void
mixclient_pipe_stop(void *arg)
{
	mixclient_pipe *p = arg;
	nni_mtx_lock(&p->mtx_send);
	nni_aio* a;
	nni_msg* m;
	while((a=nni_list_first(&p->waq))!=NULL){
		nni_aio_list_remove(a);
		nni_aio_finish_error(a,NNG_ECLOSED);
	}
	while(nni_lmq_get(&p->send_queue,&m) ==0){
		nni_msg_free(m);
	}
	nni_mtx_unlock(&p->mtx_send);
	nni_aio_stop(&p->aio_send);
	nni_aio_stop(&p->aio_recv);
	nni_aio_stop(&p->aio_put);
}

static void
mixclient_pipe_fini(void *arg)
{
	mixclient_pipe *p = arg;

	nni_aio_fini(&p->aio_send);
	nni_aio_fini(&p->aio_recv);
	nni_aio_fini(&p->aio_put);
	nni_lmq_fini(&p->send_queue);
	nni_mtx_fini(&p->mtx_send);
}

static int
mixclient_pipe_init(void *arg, nni_pipe *pipe, void *pair)
{
	mixclient_pipe *p = arg;

	nni_mtx_init(&p->mtx_send);
	nni_aio_init(&p->aio_send, mixclient_pipe_send_cb, p);
	nni_aio_init(&p->aio_recv, mixclient_pipe_recv_cb, p);
	nni_aio_init(&p->aio_put, mixclient_pipe_put_cb, p);
	nni_aio_list_init(&p->waq);
	nni_lmq_init(&p->send_queue);
	
	nni_aio_set_timeout(&p->aio_put,100); // 100 msec, this could be set dynamically?

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

static void
send_control_msg(uint8_t type, mixclient_pipe *p)
{
	// build header automatically
	nni_msg *msg;
	nni_msg_alloc(&msg, 0);
	nni_time now = nni_clock();
	if ((nni_msg_header_insert_u64(msg, now) != 0) ||
	    (nni_msg_header_append_u8(msg, NNG_MSG_URGENT) != 0) ||
	    (nni_msg_header_append_u32(msg, 0) != 0) ||
	    (nni_msg_header_append_u8(msg, NNG_SENDPOLICY_CONTROL) != 0) ||
	    (nni_msg_header_append_u8(msg, type) != 0)) {
		goto send_fail;
	}
	switch (type) {
	case NNG_MIX_CONTROL_REQUEST_NATURE:
		break;
	case NNG_MIX_CONTROL_REPLY_NATURE:
		if ((nni_msg_append(msg, &p->delay, sizeof(uint8_t)) != 0) ||
		    (nni_msg_append(msg, &p->bw, sizeof(uint8_t)) != 0) ||
		    (nni_msg_append(msg, &p->reliable, sizeof(uint8_t)) !=
		        0) ||
		    (nni_msg_append(msg, &p->security, sizeof(uint8_t)) !=
		        0)) {
			goto send_fail;
		}
		break;
	default:
		goto send_fail;
	}
	nni_mtx_lock(&p->mtx_send);
	if (p->w_ready) {
		mixclient_pipe_send(p, msg);
		nni_mtx_unlock(&p->mtx_send);
		return;
	}

	// queue it
	if (nni_lmq_full(&p->send_queue)) {
		// make space for the new msg, it will make msg lost but the control msg is the most important
		nni_msg *old;
		nni_lmq_get(&p->send_queue, &old); // should never fail
		nni_msg_free(old);
	}
	nni_lmq_put(&p->send_queue, msg); // should never fail
	nni_mtx_unlock(&p->mtx_send);
	return;

send_fail://wont unlock, pay attention
	nni_msg_free(msg);
	return;
}

static int
mixclient_pipe_start(void *arg)
{
	mixclient_pipe *p = arg;
	mixclient_sock *s = p->pair;
	uint32_t        id;
	int             rv;
	
	if (nni_pipe_peer(p->pipe) != NNG_MIXCLIENT_PEER) {
		nni_mtx_unlock(&s->mtx);
		BUMP_STAT(&s->stat_reject_mismatch);
		// Peer protocol mismatch.
		return (NNG_EPROTO);
	}
	int new_val =0;
	if((rv = nni_pipe_getopt(p->pipe, NNG_OPT_INTERFACE_DELAY,&new_val,NULL,NNI_TYPE_INT32))!=0){
		return rv;
	}else{
		p->delay = new_val;
	}
	if((rv = nni_pipe_getopt(p->pipe, NNG_OPT_INTERFACE_BW,&new_val,NULL,NNI_TYPE_INT32))!=0){
		return rv;
	}else{
		p->bw = new_val;
	}
	if((rv = nni_pipe_getopt(p->pipe, NNG_OPT_INTERFACE_RELIABLE,&new_val,NULL,NNI_TYPE_INT32))!=0){
		return rv;
	}else{
		p->reliable = new_val;
	}
	if((rv = nni_pipe_getopt(p->pipe, NNG_OPT_INTERFACE_SECURITY,&new_val,NULL,NNI_TYPE_INT32))!=0){
		return rv;
	}else{
		p->security = new_val;
	}
	nni_mtx_lock(&s->mtx);
	// add the new pipe in sock
	id = nni_pipe_id(p->pipe);
	if ((rv = nni_id_set(&s->pipes, id, p)) != 0) {
		nni_mtx_unlock(&s->mtx);
		return (rv);
	}
	nni_list_append(&s->plist, p);
	nni_mtx_unlock(&s->mtx);
	
	//start send
	mixclient_send_sched(p);
	// And the pipe read of course.
	nni_pipe_recv(p->pipe, &p->aio_recv);

	// TODO, not now,send_control_msg(NNG_MIX_CONTROL_REPLY_NATURE,p);

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

	nni_mtx_lock(&p->mtx_send);
	p->w_ready = false;
	nni_mtx_unlock(&p->mtx_send);
	nni_mtx_lock(&s->mtx);
	nni_id_remove(&s->pipes, nni_pipe_id(p->pipe));
	nni_list_node_remove(&p->node);
	nni_mtx_unlock(&s->mtx);
}

static nni_msg* generate_combined_msg(mixclient_sock*s, nni_aio* cancel_aio){
	// must be protected by mtx
	uint8_t policy;
	nni_aio* first_aio;
	nni_aio* second_aio;

	if(cancel_aio != NULL){
		if(!nni_list_active(&s->paq,cancel_aio)){
			cancel_aio = NULL;// nego has finished the cancel_aio
		}
	}
	
	nni_aio* each_aio = nni_list_first(&s->paq);
	while(each_aio != NULL){
		uint64_t tmp_time = nni_msg_header_peek_at_u64(nni_aio_get_msg(each_aio),8);
		if(tmp_time <= s->current_time){
			nni_aio_list_remove(each_aio);
			nni_aio_finish(each_aio,NNG_WMIX,0);//not real error, but inform put_cb to free the msg
			each_aio = nni_list_first(&s->paq);
			continue;
		}
		each_aio = nni_list_next(&s->paq,each_aio);
	}
	if((first_aio = nni_list_first(&s->paq)) == NULL){
		return NULL;//no aio
	}
	if((second_aio = nni_list_next(&s->paq,first_aio)) == NULL){
		//only first aio
		nni_msg* tmp_msg = nni_aio_get_msg(first_aio);
		policy           = nni_msg_header_peek_at_u8(tmp_msg, 8);
		size_t len = nni_msg_len(tmp_msg);
		nni_time tmp_time = nni_msg_header_peek_at_u64(tmp_msg,0);
		
		switch (policy) {
		case NNG_SENDPOLICY_RAW: {
			s->current_time = tmp_time;
			nni_aio_set_msg(first_aio, NULL);
			nni_aio_list_remove(first_aio);
			nni_aio_finish(first_aio,0,len);
			return tmp_msg;
		}
		case NNG_SENDPOLICY_DOUBLE: {
			if(first_aio != cancel_aio)return NULL;//wait for the next copy
			s->current_time = tmp_time;
			nni_aio_set_msg(first_aio, NULL);
			nni_aio_list_remove(first_aio);
			nni_aio_finish(first_aio,0,len);
			return tmp_msg;
		}
		case NNG_SENDPOLICY_SAMPLE: {
			// TODO
			return NULL;
		}
		case NNG_SENDPOLICY_DEFAULT: {
			// TODO
			return NULL;
		}
		default: {
			s->current_time = tmp_time;
			nni_aio_list_remove(first_aio);
			nni_aio_finish_error(first_aio, NNG_EINTR); // the error is sever
			return NULL;
		}
		}
	} else {// two put_aio
		nni_list_remove(&s->paq, second_aio);
		nni_msg* first_msg = nni_aio_get_msg(first_aio);
		nni_msg* second_msg = nni_aio_get_msg(second_aio);
		policy           = nni_msg_header_peek_at_u8(first_msg, 8);
		uint8_t second_policy = nni_msg_header_peek_at_u8(second_msg, 8);
		nni_time first_time = nni_msg_header_peek_at_u64(first_msg,0);
		nni_time second_time = nni_msg_header_peek_at_u64(second_msg,0);
		size_t first_len = nni_msg_len(first_msg);
		size_t second_len = nni_msg_len(second_msg);
		if(first_time == second_time){
			if(policy != second_policy){//should never happen
				s->current_time = first_time;
				nni_aio_list_remove(first_aio);
				nni_aio_finish_error(first_aio, NNG_EINTR); // the error is sever
				nni_aio_list_remove(second_aio);
				nni_aio_finish_error(second_aio, NNG_EINTR); // the error is sever
				return NULL;
			}
			switch (policy) {
				case NNG_SENDPOLICY_RAW: {
					s->current_time = first_time;
					nni_aio_list_remove(first_aio);
					nni_aio_finish_error(first_aio, NNG_EINTR); // the error is sever
					nni_aio_list_remove(second_aio);
					nni_aio_finish_error(second_aio, NNG_EINTR); // the error is sever
					return NULL;
				}
				case NNG_SENDPOLICY_DOUBLE: {
					s->current_time = first_time;
					nni_aio_set_msg(first_aio, NULL);
					nni_aio_list_remove(first_aio);
					nni_aio_finish(first_aio,0,first_len);
					nni_aio_set_msg(second_aio, NULL);
					nni_aio_list_remove(second_aio);
					nni_aio_finish(second_aio,0,second_len);
					nni_msg_free(second_msg);//free one
					return first_msg;//return one
				}
				case NNG_SENDPOLICY_SAMPLE: {
					// TODO
					return NULL;
				}
				case NNG_SENDPOLICY_DEFAULT: {
					// TODO
					return NULL;
				}
				// should never happen
				default: {
					s->current_time = first_time;
					nni_aio_list_remove(first_aio);
					nni_aio_finish_error(first_aio, NNG_EINTR); // the error is sever
					nni_aio_list_remove(second_aio);
					nni_aio_finish_error(second_aio, NNG_EINTR); // the error is sever
					return NULL;
				}
			}
		}else{// must finish the old one and the cancel_aio, both are bigger than current_time
			nni_aio* old_aio;
			size_t old_len;
			nni_msg* old_msg;
			nni_time old_time;
			uint8_t old_policy;
			if(first_time > second_time){
				old_aio = second_aio;
				old_len = second_aio;
				old_msg = second_aio;
				old_time = second_time;
				old_policy = second_policy;
			}else{
				old_aio = first_aio;
				old_len = first_aio;
				old_msg = first_aio;
				old_time = first_time;
				old_policy = policy;
			}
			if(policy == second_policy){//because some policy may use msgs with different timestamps
				switch (policy) {
					case NNG_SENDPOLICY_RAW:// will never happen
					case NNG_SENDPOLICY_DOUBLE: {
						if(cancel_aio == NULL || old_aio == cancel_aio){
							s->current_time = old_time;
							nni_aio_set_msg(old_aio, NULL);
							nni_aio_list_remove(old_aio);
							nni_aio_finish(old_aio,0,old_len);//not real error, but inform put_cb to free the msg
							return old_msg;
						}
						nni_aio_list_remove(old_aio);
						nni_aio_finish(old_aio,NNG_WMIX,0);//not real error, but inform put_cb to free the msg
						nni_msg* cancel_msg = nni_aio_get_msg(cancel_aio);
						s->current_time = nni_msg_header_peek_at_u64(cancel_msg,0);
						nni_aio_set_msg(cancel_aio, NULL);
						nni_aio_list_remove(cancel_aio);
						nni_aio_finish(cancel_aio,0, nni_msg_len(cancel_msg);
						return cancel_msg;
					}
					case NNG_SENDPOLICY_SAMPLE: {
						// TODO
						return NULL;
					}
					case NNG_SENDPOLICY_DEFAULT: {
						// TODO
						return NULL;
					}
					// should never happen
					default: {
						s->current_time = first_time > second_time?first_time:second_time;
						nni_aio_list_remove(first_aio);
						nni_aio_finish_error(first_aio, NNG_EINTR); // the error is sever
						nni_aio_list_remove(second_aio);
						nni_aio_finish_error(second_aio, NNG_EINTR); // the error is sever
						return NULL;
					}
				}
			}else{// must finish the old one
				switch (old_policy) {
					case NNG_SENDPOLICY_RAW:
					case NNG_SENDPOLICY_DOUBLE: {
						if(cancel_aio == NULL || old_aio == cancel_aio){
							s->current_time = old_time;
							nni_aio_set_msg(old_aio, NULL);
							nni_aio_list_remove(old_aio);
							nni_aio_finish(old_aio,0,old_len);//not real error, but inform put_cb to free the msg
							return old_msg;
						}
						nni_aio_list_remove(old_aio);
						nni_aio_finish(old_aio,NNG_WMIX,0);//not real error, but inform put_cb to free the msg
						nni_msg* cancel_msg = nni_aio_get_msg(cancel_aio);
						s->current_time = nni_msg_header_peek_at_u64(cancel_msg,0);
						nni_aio_set_msg(cancel_aio, NULL);
						nni_aio_list_remove(cancel_aio);
						nni_aio_finish(cancel_aio,0, nni_msg_len(cancel_msg);
						return cancel_msg;
					}
					case NNG_SENDPOLICY_SAMPLE: {
						// TODO
						return NULL;
					}
					case NNG_SENDPOLICY_DEFAULT: {
						// TODO
						return NULL;
					}
					// should never happen
					default: {
						s->current_time = first_time > second_time?first_time:second_time;
						nni_aio_list_remove(first_aio);
						nni_aio_finish_error(first_aio, NNG_EINTR); // the error is sever
						nni_aio_list_remove(second_aio);
						nni_aio_finish_error(second_aio, NNG_EINTR); // the error is sever
						return NULL;
					}
				}
			}
		}
	}
}

static void
nego_combined_msg(mixclient_sock *s,nni_aio* cancel_aio){// aio_put and reader aio must be in the waiting list already
	nni_aio* reader;
	nni_msg *new = generate_combined_msg(s,cancel_aio);
	while((reader = nni_list_first(&s->raq))!=NULL){
		if (!nni_lmq_empty(&s->urq)) {
			nni_msg *old;
			nni_lmq_get(&s->urq, &old); // should never fail
			nni_aio_list_remove(reader);
			nni_aio_set_msg(reader,old);
			nni_aio_finish_sync(reader,0,nni_msg_len(old));
			if(new != NULL){
				nni_lmq_put(&s->urq, new); // should never fail
				new = NULL;
			}
			continue;
		}
		if(new != NULL){
			nni_aio_list_remove(reader);
			nni_aio_set_msg(reader,new);
			nni_aio_finish_sync(reader,0,nni_msg_len(new));
			new = NULL;
			continue;
		}
		// no msg available, and generate fail, we should not generate again
		return;
	}
	//no more reader
	if(new == NULL)return;
	if (nni_lmq_full(&s->urq)) {
		// make space for the new msg
		nni_msg *old;
		nni_lmq_get(&s->urq, &old); // should never fail
		nni_msg_free(old);
		nni_lmq_put(&s->urq, new); // should never fail
		return;
	} else{ // have space in lmq
		nni_lmq_put(&s->urq, new);    // should never fail
		return;
	}
}

static void
mixclient_pipe_put_cancel(nni_aio *aio, void *arg, int rv)
{
	mixclient_sock *s = arg;

	nni_mtx_lock(&s->mtx_urq);
	//if rv is NNG_ECLOSED or NNG_ECANCELED, we can still use the msg in aio_put and finish it normally, because nni_pipe_recv will help stop the recv progress
	if (nni_aio_list_active(aio)) {
		nego_combined_msg(s,aio);
	} // if not, means the aio is finished by other pipe
	nni_mtx_unlock(&s->mtx_urq);
}

static void
mixclient_pipe_put_cb(void *arg)
{
	mixclient_pipe *p = arg;

	int rv = nni_aio_result(&p->aio_put);
	if(rv == 0){// if the pipe has been closed but we return 0, nni_pipe_recv will just return and do nothing
		nni_pipe_recv(p->pipe, &p->aio_recv);
	}else if(rv == NNG_WMIX){
		nni_msg_free(nni_aio_get_msg(&p->aio_put));
		nni_aio_set_msg(&p->aio_put, NULL);
		nni_pipe_recv(p->pipe, &p->aio_recv);
	}else{
		nni_msg_free(nni_aio_get_msg(&p->aio_put));
		nni_aio_set_msg(&p->aio_put, NULL);
		nni_pipe_close(p->pipe);
	}
	return;
}

static void
mixclient_pipe_recv_cb(void *arg)
{
	mixclient_pipe *p = arg;
	mixclient_sock *s = p->pair;
	nni_msg *       msg;
	nni_pipe *      pipe = p->pipe;
	size_t          len;
	int             rv;

	if (nni_aio_result(&p->aio_recv) != 0) {
		nni_pipe_close(p->pipe);
		return;
	}

	msg = nni_aio_get_msg(&p->aio_recv);
	nni_aio_set_msg(&p->aio_recv, NULL);

	// Store the pipe ID.
	nni_msg_set_pipe(msg, nni_pipe_id(p->pipe));

	if (nni_msg_len(msg) < 9) {
		goto msg_error;	
	}
	//move header from body to header
	uint8_t send_policy = nni_msg_peek_at_u8(msg,8);
	int policy_specified_len = 0;
	switch(send_policy){
		case NNG_SENDPOLICY_RAW:{
			policy_specified_len = 1;
			break;
		}
		case NNG_SENDPOLICY_DOUBLE:{
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
			goto msg_error;	
		}
	}
	nni_msg_header_insert(msg,nni_msg_body(msg),9+policy_specified_len)
	if(nni_msg_trim(msg,9+policy_specified_len) !=0){
		goto msg_error;	
	}
	len = nni_msg_len(msg);
	nni_sock_bump_rx(s->sock, len);

	// Send the message up. Aio_put should be inserted first before nego
	nni_aio_set_msg(&p->aio_put, msg);
	if((rv = nni_aio_begin(&p->aio_put)) != 0){// the pipe has been closed, should not happen
		nni_msg_free(msg);
		return;
	}
	nni_mtx_lock(&s->mtx_urq);
	nni_aio_list_append(&s->paq, &p->aio_put);// aio_begin must be before list append
	nego_combined_msg(s,NULL);// for performance, we nego first, if fail, we schedule the aio
	if(nni_aio_list_active(&p->aio_put)){//still in paq means fail
		if ((rv = nni_aio_schedule(&p->aio_put, mixclient_pipe_put_cancel, s)) !=0) {
			nni_aio_finish_error(&p->aio_put, rv);//should only happen when the pipe is closed
			nni_mtx_unlock(&s->mtx_urq);
			return;
		}
		//start wait
	}
	nni_mtx_unlock(&s->mtx_urq);
	return;// pub_cb has run pipe_recv already
msg_error:
	BUMP_STAT(&s->stat_rx_malformed);
	nni_msg_free(msg);
	nni_pipe_close(pipe);// dont need to pipe_recv
	return;
}

static void 
mixclient_pipe_send(mixclient_pipe *p, nni_msg *m){
	NNI_ASSERT(!nni_msg_shared(m));
	nni_aio_set_msg(&p->aio_send,m);
	nni_pipe_send(p->pipe,&p->aio_send);
	p->w_ready = false;
}

static void
mixclient_send_sched(mixclient_pipe* p){
	if(p == NULL)return;
	nni_msg *m;
	nni_aio *a;
	size_t l = 0;
	nni_mtx_lock(&p->mtx_send);
	p->w_ready = true;
	if(nni_lmq_get(&p->send_queue,&m) ==0){
		mixclient_pipe_send(p,m);
		if((a = nni_list_first(&p->waq))!=NULL){
			nni_aio_list_remove(a);
			m = nni_aio_get_msg(a);
			l = nni_msg_len(m);
			nni_lmq_put(&p->send_queue,m);
		}
	}else if((a=nni_list_first(&p->waq))!=NULL){
		nni_aio_list_remove(a);
		m = nni_aio_get_msg(a);
		l = nni_msg_len(m);
		mixclient_pipe_send(p,m);
	}
	nni_mtx_unlock(&p->mtx_send);
	if(a != NULL){
		nni_aio_set_msg(a,NULL);
		nni_aio_finish_sync(a,0,l);
	}
	// w_ready is true, wait for write aio
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

	mixclient_send_sched(p);
}

static void
mixclient_send_cancel(nni_aio *aio, void *arg, int rv)
{
	mixclient_pipe *p = arg;

	nni_mtx_lock(&p->mtx_send);// aio is in the pipe's send_queue, so just lock pipe
	if (nni_aio_list_active(aio)) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&p->mtx_send);
}

static void
mixclient_recv_cancel(nni_aio *aio, void *arg, int rv)
{
	mixclient_sock *s = arg;

	nni_mtx_lock(&s->mtx_urq);// aio is in the sock's raq, so lock urq
	if (nni_aio_list_active(aio)) {
		nni_aio_list_remove(aio);
		nni_aio_finish_error(aio, rv);
	}
	nni_mtx_unlock(&s->mtx_urq);
}

static void
get_two_pipes(mixclient_pipe** first, mixclient_pipe**second,mixclient_sock*s){
//TODO
	nni_mtx_lock(&s->mtx);
	mixclient_pipe* first_pipe = nni_list_first(&s->plist);
	if(first_pipe == NULL){
		nni_mtx_unlock(&s->mtx);
		goto send_fail;
	}
	mixclient_pipe* second_pipe = nni_list_next(&s->plist,first_pipe);
	if(second_pipe == NULL){
		nni_mtx_unlock(&s->mtx);
		goto send_fail;
	}
	nni_mtx_unlock(&s->mtx);
}

static void
mixclient_sock_send(void *arg, nni_aio *aio)// the rv is handled by user, dont need to remove aio from list
{
	mixclient_sock *s = arg;
	int rv;

	nni_msg *msg= nni_aio_get_msg(aio);
	size_t len = nni_msg_len(msg);
	nni_sock_bump_tx(s->sock, len);

	if (nni_aio_begin(aio) != 0) {
		return;
	}
	if(nni_msg_header_len(msg) < 6){
			BUMP_STAT(&s->stat_tx_malformed);
			goto send_fail;
	}
	uint8_t send_policy = nni_msg_header_peek_at_u8(msg,5);
	switch(send_policy){
	case NNG_SENDPOLICY_RAW:{
		mixclient_pipe *p;
		if(nni_msg_header_len(msg) != 7){
			BUMP_STAT(&s->stat_tx_malformed);
			goto send_fail;
		}
		//uint8_t urgency_level = nni_msg_header_peek_u8(msg);
		uint8_t nature_chosen = nni_msg_header_peek_at_u8(msg,6);
		
		uint32_t id = nni_msg_get_pipe(msg);
		nni_time now = nni_clock();
		if(nni_msg_header_insert_u64(msg, now) != 0){
			goto send_fail;
		}
		nni_mtx_lock(&s->mtx);
		if(id != 0){
			if((p = nni_id_get(&s->pipes,id) == NULL){
				BUMP_STAT(&s->stat_tx_drop);
				nni_mtx_unlock(&s->mtx);
				goto send_fail;
			}
		}else{
			mixclient_pipe* first_pipe = nni_list_first(&s->plist);
			if(first_pipe == NULL){
				BUMP_STAT(&s->stat_tx_drop);
				nni_mtx_unlock(&s->mtx);
				goto send_fail;
			}
			mixclient_pipe* second_pipe = nni_list_next(&s->plist,first_pipe);
			if(second_pipe == NULL){
				p = first_pipe;
			} else {
				switch (nature_chosen) {
				case NNG_MIX_RAW_DELAY:
					p = first_pipe->delay > second_pipe->delay?first_pipe:second_pipe;
					break;
				case NNG_MIX_RAW_BW:
					p = first_pipe->bw > second_pipe->bw?first_pipe:second_pipe;
					break;
				case NNG_MIX_RAW_RELIABLE:
					p = first_pipe->reliable > second_pipe->reliable?first_pipe:second_pipe;
					break;
				// Other msgs use the safest pipe
				default:
					p = first_pipe->security > second_pipe->security?first_pipe:second_pipe;
					break;
				}
			}
		}
		nni_mtx_unlock(&s->mtx);//we find that pipe
		nni_mtx_lock(&p->mtx_send);
		if(p->w_ready){
			nni_aio_set_msg(aio,NULL);
			nni_aio_finish(aio,0,len);
			mixclient_pipe_send(p,msg);
			nni_mtx_unlock(&p->mtx_send);
			return;
		}

		// queue it
		if(nni_lmq_put(&p->send_queue,msg) == 0){
			nni_aio_set_msg(aio,NULL);
			nni_aio_finish(aio,0,len);
			nni_mtx_unlock(&p->mtx_send);
			return;
		}
		// let aio wait
		if((rv = nni_aio_schedule(aio,mixclient_send_cancel,p))!=0){
			nni_aio_finish_error(aio,rv);
			nni_mtx_unlock(&p->mtx_send);
			return;
		}
		nni_aio_list_append(&p->waq,aio);
		nni_mtx_unlock(&p->mtx_send);
		break;
	}
	case NNG_SENDPOLICY_DOUBLE:{
		if(nni_msg_header_len(msg) != 6){
			BUMP_STAT(&s->stat_tx_malformed);
			goto send_fail;
		}
		nni_time now = nni_clock();
		if(nni_msg_header_insert_u64(msg, now) != 0){
			goto send_fail;
		}
		nni_mtx_lock(&s->mtx);
		mixclient_pipe* first_pipe = nni_list_first(&s->plist);
		if(first_pipe == NULL){
			nni_mtx_unlock(&s->mtx);
			goto send_fail;
		}
		mixclient_pipe* second_pipe = nni_list_next(&s->plist,first_pipe);
		if(second_pipe == NULL){
			nni_mtx_unlock(&s->mtx);
			goto send_fail;
		}
		nni_mtx_unlock(&s->mtx);
		
		// the first pipe send
		nni_msg *dup_m = NULL;
		if(nni_msg_dup(&dup_m,msg) ==0){
			goto send_fail;
		}
		bool dup_m_used = false;
		bool aio_used = false;
		nni_mtx_lock(&first_pipe->mtx_send);
		if(first_pipe->w_ready){
			//use the dup_m first
			mixclient_pipe_send(first_pipe,dup_m);
			dup_m_used = true;
		}else if(nni_lmq_put(&first_pipe->send_queue,dup_m) == 0){
			dup_m_used = true;
		}
		if(!dup_m_used){
			// let aio wait, we dont care if the NONBLOCK is set
			rv = nni_aio_schedule(aio,mixclient_send_cancel,first_pipe);
			if(rv == NNG_ECLOSED){
				rv = NNG_WMIX;
			}else if(rv == 0){
				nni_aio_list_append(&first_pipe->waq,aio);
				aio_used = true;
			}// rv is TIMEOUT, wont let aio wait
			nni_mtx_unlock(&first_pipe->mtx_send);
		}

		nni_mtx_lock(&second_pipe->mtx_send);
		if(second_pipe->w_ready){
			if(dup_m_used){
				nni_aio_set_msg(aio,NULL);
				nni_aio_finish(aio,0,len);//all used means success
				mixclient_pipe_send(second_pipe,msg);
				nni_mtx_unlock(&second_pipe->mtx_send);
				return;
			}
			mixclient_pipe_send(p,dup_m);
			if(aio_used){
				nni_mtx_unlock(&second_pipe->mtx_send);
				return;
			}
			// aio is not used(ECLOSED or TIMEOUT) and is useless
			nni_msg_free(msg);
			nni_aio_set_msg(aio,NULL);
			nni_aio_finish(aio,0,len);// success, but free the msg 
			nni_mtx_unlock(&second_pipe->mtx_send);
			return;
		}
		if(dup_m_used){
			if(nni_lmq_put(&second_pipe->send_queue,msg) == 0){
				nni_aio_set_msg(aio,NULL);
				nni_aio_finish(aio,0,len);//all used means success
				nni_mtx_unlock(&second_pipe->mtx_send);
				return;
			}
			// let aio wait, we dont care if the NONBLOCK is set
			rv = nni_aio_schedule(aio,mixclient_send_cancel,second_pipe);
			if(rv == NNG_ECLOSED){
				nni_msg_free(msg);
				nni_aio_set_msg(aio,NULL);
				nni_aio_finish(aio,0,len);// first pipe success, so success
				nni_mtx_unlock(&second_pipe->mtx_send);
				return;
			}else if(rv == 0){
				nni_aio_list_append(&second_pipe->waq,aio);
				nni_mtx_unlock(&second_pipe->mtx_send);
				return;
			}// TIMEOUT, wont let aio wait
			nni_msg_free(msg);
			nni_aio_set_msg(aio,NULL);
			nni_aio_finish(aio,0,len);// first pipe success
			nni_mtx_unlock(&second_pipe->mtx_send);
			return;
		}else{
			if(aio_used){
				if(nni_lmq_put(&second_pipe->send_queue,dup_m) == 0){
					nni_mtx_unlock(&second_pipe->mtx_send);
					return;
				}
				nni_msg_free(dup_m);
				nni_mtx_unlock(&second_pipe->mtx_send);
				return;
			}else{//the first pipe is closed or TIMEOUT
				nni_msg_free(dup_m);// dup_m will not be used
				if(nni_lmq_put(&second_pipe->send_queue,msg) == 0){
					nni_aio_set_msg(aio,NULL);
					nni_aio_finish(aio,0,len);
					nni_mtx_unlock(&second_pipe->mtx_send);
					return;
				}
				// let aio wait
				rv = nni_aio_schedule(aio,mixclient_send_cancel,second_pipe);
				if(rv == NNG_ECLOSED){
					nni_aio_finish_error(aio,rv);// both fail
					nni_mtx_unlock(&second_pipe->mtx_send);
					return;
				}else if(rv == 0){
					nni_aio_list_append(&second_pipe->waq,aio);
					nni_mtx_unlock(&second_pipe->mtx_send);
					return;
				}// TIMEOUT, wont let aio wait
				nni_aio_finish_error(aio,rv);// both fail
				nni_mtx_unlock(&second_pipe->mtx_send);
				return;
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
	//nni_aio_set_msg(aio, NULL);
	//nni_aio_finish(aio,0,len);
	return;
send_fail:
	//if fail, we dont get the msg's ownership
	nni_aio_finish_error(aio,NNG_EINVAL);//NNG_EINVAL maybe not so clear
	return;
}

static void
mixclient_sock_recv(void *arg, nni_aio *aio)
{
	mixclient_sock *s = arg;
	nni_msg* m;
	int rv;
	nni_aio*a;
	if(nni_aio_begin(aio) != 0){
		return;
	}
	nni_mtx_lock(&s->mtx_urq);
	nni_aio_list_append(&s->raq, aio);// add first
	nego_combined_msg(s,NULL);
	if(nni_aio_list_active(aio)){// still in raq means fail
		if ((rv = nni_aio_schedule(aio, mixclient_recv_cancel, s)) != 0) {
			nni_aio_finish_error(aio, rv);
			nni_mtx_unlock(&s->mtx_urq);
			return;
		}
	}
	nni_mtx_unlock(&s->mtx_urq);
	return;
}
/*
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
*/
static nni_proto_pipe_ops mixclient_pipe_ops = {
	.pipe_size  = sizeof(mixclient_pipe),
	.pipe_init  = mixclient_pipe_init,
	.pipe_fini  = mixclient_pipe_fini,
	.pipe_start = mixclient_pipe_start,
	.pipe_close = mixclient_pipe_close,
	.pipe_stop  = mixclient_pipe_stop,
};

static nni_option mixclient_sock_options[] = {
	/*{
	    .o_name = NNG_OPT_MIX_SENDPOLICY,
	    .o_get  = mixclient_get_send_policy,
	    .o_set  = mixclient_set_send_policy,
	},
	{
	    .o_name = NNG_OPT_MIX_RECVPOLICY,
	    .o_get  = mixclient_get_recv_policy,
	    .o_set  = mixclient_set_recv_policy,
	},*/
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
