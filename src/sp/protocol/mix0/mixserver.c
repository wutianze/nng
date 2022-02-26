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

static void mixserver_pipe_send_cb(void *);
static void mixserver_pipe_recv_cb(void *);
static void mixserver_pipe_get_cb(void *);
static void mixserver_pipe_put_cb(void *);
static void mixserver_pipe_fini(void *);
static void mixserver_app_put_cb(void *);
static void mixserver_app_get_cb(void *);
// mixserver_sock is our per-socket protocol private structure.
struct mixserver_sock {
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
	
	nni_id_map     apps;

	int            recv_wait;
#ifdef NNG_ENABLE_STATS
	nni_stat_item  stat_mixserver;
	nni_stat_item  stat_reject_mismatch;
	nni_stat_item  stat_reject_already;
	nni_stat_item  stat_rx_malformed;
	nni_stat_item  stat_tx_malformed;
	nni_stat_item  stat_tx_drop;
#endif
};

struct mixserver_app{
	nni_list     plist;//related pipes
	mixserver_sock *pair;

	nni_mtx      mtx_urq;
	nni_list     paq;// aio_put from pipes
	nni_lmq      urq;
	bool         aio_get_ready;
	
	nni_aio      aio_put;// put msg to upper recv_queue
	nni_aio      aio_get;//get msg from urq
	nni_mtx      mtx;
	uint32_t     id;
	nni_time     current_time;
	bool         aio_get_ready;
};

// mixserver_pipe is our per-pipe protocol private structure.
struct mixserver_pipe {
	nni_pipe       *pipe;
	mixserver_sock *pair;
	mixserver_app  *app;

	nni_mtx         mtx_send;
	nni_lmq       	send_queue;
	nni_list        waq;
	
	nni_aio         aio_send;
	nni_aio         aio_recv;
	
	nni_aio         aio_put;

	bool            w_ready;

	// point to the app the pipe belongs to
	// be initialized once the first msg received
	// won't be deleted when this pipe is closed
	// stored in the map in sock

	//may need a mtx later
	uint8_t delay;
	uint8_t bw;
	uint8_t reliable;
	uint8_t security;

	nni_list_node    node;
};



static void
mixserver_app_init(void *arg){
	mixserver_app *a = arg;
	nni_aio_init(&a->aio_get,mixserver_app_get_cb,a);
	nni_aio_init(&a->aio_put,mixserver_app_put_cb,a);
	nni_lmq_init(&a->readyq,0);
	nni_aio_list_init(waq);
	NNI_LIST_INIT(&a->plist, mixserver_pipe,node);
	nni_mtx_init(&a->mtx);
}

static void
mixserver_sock_fini(void *arg)
{
	mixserver_sock *s = arg;

	nni_id_map_fini(&s->pipes);
	nni_id_map_fini(&s->apps);
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

static void
mixserver_sock_init(void *arg, nni_sock *sock)
{
	mixserver_sock *s = arg;

	nni_id_map_init(&s->pipes, 0, 0, false);
	nni_id_map_init(&s->apps, 0, 0, false);
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

	//give server more buffer, panic if no enough memory
	//TODO let user check before using it.
	NNI_ASSERT(nni_msgq_init(&s->urq_urgent,8)==0);
	NNI_ASSERT(nni_msgq_init(&s->urq_normal,8)==0);
	NNI_ASSERT(nni_msgq_init(&s->urq_unimportant,8)==0);
	
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
	p->app = NULL;

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
	p->app_list = NULL;
}

static nni_msg* generate_combined_msg(mixserver_app*app, nni_aio* cancel_aio){
	// must be protected by mtx
	uint8_t policy;
	nni_aio* first_aio;
	nni_aio* second_aio;

	if(cancel_aio != NULL){
		if(!nni_list_active(&app->paq,cancel_aio)){
			cancel_aio = NULL;// nego has finished the cancel_aio
		}
	}
	
	nni_aio* each_aio = nni_list_first(&app->paq);
	while(each_aio != NULL){
		uint64_t tmp_time = nni_msg_header_peek_at_u64(nni_aio_get_msg(each_aio),8);
		if(tmp_time <= s->current_time){
			nni_aio_list_remove(each_aio);
			nni_aio_finish(each_aio,NNG_WMIX,0);//not real error, but inform put_cb to free the msg
			each_aio = nni_list_first(&app->paq);
			continue;
		}
		each_aio = nni_list_next(&app->paq,each_aio);
	}
	if((first_aio = nni_list_first(&app->paq)) == NULL){
		return NULL;//no aio
	}
	if((second_aio = nni_list_first(&app->paq)) == NULL){
		//only first aio
		nni_msg* tmp_msg = nni_aio_get_msg(first_aio);
		policy           = nni_msg_header_peek_at_u8(tmp_msg, 8);
		size_t len = nni_msg_len(tmp_msg);
		nni_time tmp_time = nni_msg_header_peek_at_u64(tmp_msg,0);
		
		switch (policy) {
		case NNG_SENDPOLICY_RAW: {
			app->current_time = tmp_time;
			nni_aio_set_msg(first_aio, NULL);
			nni_aio_list_remove(first_aio);
			nni_aio_finish(first_aio,0,len);
			return tmp_msg;
		}
		case NNG_SENDPOLICY_DOUBLE: {
			if(first_aio != cancel_aio)return NULL;//wait for the next copy
			app->current_time = tmp_time;
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
			app->current_time = tmp_time;
			nni_aio_list_remove(first_aio);
			nni_aio_finish_error(first_aio, NNG_EINTR); // the error is sever
			return NULL;
		}
		}
	} else {// two put_aio
		nni_list_remove(&app->paq, second_aio);
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
				app->current_time = first_time;
				nni_aio_list_remove(first_aio);
				nni_aio_finish_error(first_aio, NNG_EINTR); // the error is sever
				nni_aio_list_remove(second_aio);
				nni_aio_finish_error(second_aio, NNG_EINTR); // the error is sever
				return NULL;
			}
			switch (policy) {
				case NNG_SENDPOLICY_RAW: {
					app->current_time = first_time;
					nni_aio_list_remove(first_aio);
					nni_aio_finish_error(first_aio, NNG_EINTR); // the error is sever
					nni_aio_list_remove(second_aio);
					nni_aio_finish_error(second_aio, NNG_EINTR); // the error is sever
					return NULL;
				}
				case NNG_SENDPOLICY_DOUBLE: {
					app->current_time = first_time;
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
					app->current_time = first_time;
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
							app->current_time = old_time;
							nni_aio_set_msg(old_aio, NULL);
							nni_aio_list_remove(old_aio);
							nni_aio_finish(old_aio,0,old_len);//not real error, but inform put_cb to free the msg
							return old_msg;
						}
						nni_aio_list_remove(old_aio);
						nni_aio_finish(old_aio,NNG_WMIX,0);//not real error, but inform put_cb to free the msg
						nni_msg* cancel_msg = nni_aio_get_msg(cancel_aio);
						app->current_time = nni_msg_header_peek_at_u64(cancel_msg,0);
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
						app->current_time = first_time > second_time?first_time:second_time;
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
							app->current_time = old_time;
							nni_aio_set_msg(old_aio, NULL);
							nni_aio_list_remove(old_aio);
							nni_aio_finish(old_aio,0,old_len);//not real error, but inform put_cb to free the msg
							return old_msg;
						}
						nni_aio_list_remove(old_aio);
						nni_aio_finish(old_aio,NNG_WMIX,0);//not real error, but inform put_cb to free the msg
						nni_msg* cancel_msg = nni_aio_get_msg(cancel_aio);
						app->current_time = nni_msg_header_peek_at_u64(cancel_msg,0);
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
						app->current_time = first_time > second_time?first_time:second_time;
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
nego_combined_msg(mixserver_app *app,nni_aio* cancel_aio){// pipe's aio_puts and app's aio_get must be in the waiting list already
	nni_msg *new = generate_combined_msg(app,cancel_aio);
	if(app->aio_get_ready){
		if (!nni_lmq_empty(&app->urq)) {
			nni_msg *old;
			nni_lmq_get(&app->urq, &old); // should never fail
			app->aio_get_ready = false;
			nni_aio_set_msg(app->aio_get,old);
			nni_aio_finish_sync(app->aio_get,0,nni_msg_len(old));
			if(new != NULL){
				nni_lmq_put(&app->urq, new); // should never fail
				new = NULL
			}
			return;
		}
		if(new != NULL){
			nni_aio_set_msg(app->aio_get,new);
			app->aio_get_ready = false;
			nni_aio_finish_sync(app->aio_get,0,nni_msg_len(new));
			new = NULL;
			return;
		}
		// no msg available, and generate fail, we should not generate again
		return;
	}
	//no more reader
	if(new == NULL)return;
	if (nni_lmq_full(&app->urq)) {
		// make space for the new msg
		nni_msg *old;
		nni_lmq_get(&app->urq, &old); // should never fail
		nni_msg_free(old);
		nni_lmq_put(&app->urq, new); // should never fail
		return;
	} else{ // have space in lmq
		nni_lmq_put(&app->urq, new);    // should never fail
		return;
	}
}

static void
mixserver_pipe_recv_cb(void *arg)
{
	mixserver_pipe *p = arg;
	mixserver_sock *s = p->pair;
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

	if (nni_msg_len(msg) < 14) {
		goto msg_error;	
	}

	//move header from body to header
	uint8_t send_policy = nni_msg_peek_at_u8(msg,13);
	int policy_specified_len = 0;
	switch(send_policy){
		case NNG_SENDPOLICY_RAW:{
			policy_specified_len = 1;
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
	nni_msg_header_insert(msg,nni_msg_body(msg),14+policy_specified_len)
	if(nni_msg_trim(msg,14+policy_specified_len) !=0){
		goto msg_error;	
	}
	uint32_t app_id = nni_msg_header_peek_at_u32(msg,9);


	//check if the app exists already TODO, allocated automatically, not now
	if(p->app != NULL){// the pipe is bind to an app already
		if(p->app->id != app_id){
			goto msg_error; // we dont allow app id change
		}
	}else{
		nni_mtx_lock(&s->mtx);
		mixserver_app* app_find = nni_id_get(&s->apps,app_id);
		if(app_find == NULL){// not exist, create a new app
			mixserver_app* new_app = nni_alloc(sizeof(mixserver_app));
			if(new_app == NULL){
				NNI_ASSERT(new_app != NULL);
				goto msg_error;//shouldnt be msg error, but this is rarely happen
			}
			mixserver_app_init(new_app);
			nni_id_set(&s->apps,app_id,new_app);
			p->app = new_app;
			new_app->id = app_id;
		}else{//another pipe of the app has created the app
			p->app = app_find;
		}
		nni_mtx_unlock(&s->mtx);
	}

	len = nni_msg_len(msg);
	nni_sock_bump_rx(s->sock, len);// after this, shouldnt have another msg error

	nni_aio_set_msg(&p->aio_put, msg);
	if((rv = nni_aio_begin(&p->aio_put)) != 0){// the pipe has been closed, should not happen
		nni_msg_free(msg);
		return;
	}
	
	mixserver_app  *a = p->app;
	nni_mtx_lock(&a->mtx_urq);
	nni_aio_list_append(&a->paq, &p->aio_put);// aio_begin must be before list append
	nego_combined_msg(a,NULL);// for performance, we nego first, if fail, we schedule the aio
	if(nni_aio_list_active(&p->aio_put)){//still in paq means fail
		if ((rv = nni_aio_schedule(&p->aio_put, mixserver_pipe_put_cancel, a)) !=0) {
			nni_aio_finish_error(&p->aio_put, rv);//should only happen when the pipe is closed
			nni_mtx_unlock(&a->mtx_urq);
			return;
		}
		//start wait
	}
	nni_mtx_unlock(&a->mtx_urq);

	return;
msg_error:
	BUMP_STAT(&s->stat_rx_malformed);
	nni_msg_free(msg);
	nni_pipe_close(pipe);
	return;
}

static void
mixserver_pipe_put_cancel(nni_aio *aio, void *arg, int rv)
{
	mixserver_app *app = arg;

	nni_mtx_lock(&app->mtx_urq);
	//if rv is NNG_ECLOSED or NNG_ECANCELED, we can still use the msg in aio_put and finish it normally, because nni_pipe_recv will help stop the recv progress
	if (nni_aio_list_active(aio)) {
		nego_combined_msg(app,aio);
	} // if not, means the aio is finished by other pipe
	nni_mtx_unlock(&app->mtx_urq);
}

static void
mixserver_pipe_put_cb(void *arg)
{
	mixserver_pipe *p = arg;
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
mixserver_app_put_cb(void *arg)
{
	mixserver_app *a = arg;
	nni_msg *       msg;

	NNI_ASSERT(nni_aio_result(&a->aio_put) == 0);// strange error, should never happen
	nni_mtx_lock(&a->mtx_urq);
	a->aio_get_ready = true;
	nni_mtx_unlock(&a->mtx_urq);
}

static void
mixserver_app_get_cb(void *arg)
{
	mixserver_app *a = arg;
	nni_msg *       msg;

	NNI_ASSERT(nni_aio_result(&a->aio_get) == 0);

	msg = nni_aio_get_msg(&a->aio_get);
	nni_aio_set_msg(&a->aio_get, NULL);

	nni_aio_set_msg(&a->aio_put, msg);
	uint8_t urgency = nni_msg_header_peek_at_u8(msg, 8);
	switch (urgency)
	{
		case NNG_MSG_URGENT:
			nni_msgq_aio_put(a->pair->urq_urgent,&a->aio_put);
			break;
		case NNG_MSG_NORMAL:
			nni_msgq_aio_put(a->pair->urq_normal,&a->aio_put);
			break;
		default:
			nni_msgq_aio_put(a->pair->urq_unimportant,&a->aio_put);
			break;
	}
}

static void 
mixserver_pipe_send(mixserver_pipe *p, nni_msg *m){
	NNI_ASSERT(!nni_msg_shared(m));
	nni_aio_set_msg(&p->aio_send,m);
	nni_pipe_send(p->pipe,&p->aio_send);
	p->w_ready = false;
}

static void
mixserver_send_sched(mixserver_pipe* p){
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

	mixserver_send_sched(p);
}

static void
get_two_pipes(mixserver_pipe** first, mixserver_pipe**second,mixserver_app*a){
	*first = nni_list_first(&a->plist);
	if(*first == NULL){
		*second = NULL;
		return;
	}
	*second = nni_list_next(&a->plist,*first);
	return;
}

static void
mixserver_sock_send(void *arg, nni_aio *aio) //TODO
{
	mixserver_sock *s = arg;
	int rv;

	nni_msg *msg= nni_aio_get_msg(aio);
	size_t len = nni_msg_len(msg);
	nni_sock_bump_tx(s->sock, len);

	if (nni_aio_begin(aio) != 0) {
		return;
	}
	if(nni_msg_header_len(msg) < 5){
			BUMP_STAT(&s->stat_tx_malformed);
			goto send_fail;
	}
	uint8_t send_policy = nni_msg_header_peek_at_u8(msg,4);
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
		mixclient_pipe* first_pipe = nni_list_first(&s->delay_list);
		mixclient_pipe* second_pipe = nni_list_next(&s->delay_list,first_pipe);
		if(first_pipe == NULL || second_pipe == NULL){
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
	
	nni_msgq_aio_get(s->urq_urgent,aio);//only wait on urgent queue, should have a timeout handler
	return;
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
