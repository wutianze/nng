// Copyright 2018 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2018 Capitar IT Group BV <info@capitoar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

// This program is just a simple client application for our demo server.
// It is in a separate file to keep the server code clearer to understand.
//
// Our demonstration application layer protocol is simple.  The client sends
// a number of milliseconds to wait before responding.  The server just gives
// back an empty reply after waiting that long.

//  For example:
//
//  % ./server tcp://127.0.0.1:5555 &
//  % ./client tcp://127.0.0.1:5555 323
//  Request took 324 milliseconds.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <nng/nng.h>
#include <nng/protocol/mix0/mixclient.h>
#include <nng/supplemental/util/platform.h>

void
fatal(const char *func, int rv)
{
	fprintf(stderr, "%s: %s\n", func, nng_strerror(rv));
	exit(1);
}

/*  The client runs just once, and then returns. */
int
client(const char *url, const char *msecstr, const char* numstr)
{
	nng_socket sock;
	int        rv;
	nng_msg *  msg;
	nng_time   start;
	nng_time   end;
	unsigned   msec;
	unsigned   num;

	msec = atoi(msecstr);
	num = atoi(numstr);

	if ((rv = nng_mixclient_open(&sock)) != 0) {
		fatal("nng_mix_open", rv);
	}
	
	nng_socket_set_int(sock,NNG_OPT_MIX_SENDPOLICY,NNG_SENDPOLICY_RAW);

        //first dialer
        nng_dialer tmpd;//only an id, dont worry to pass by value
        if ((rv = nng_dialer_create(&tmpd,sock, url)) != 0) {
                fatal("nng_dialer_create", rv);
        }
        if((rv = nng_dialer_set_int(tmpd,NNG_OPT_INTERFACE_DELAY,4))!=0){
                fatal("nng_dialer set INTERFACE_DELAY", rv);
        }
        if((rv = nng_dialer_set_int(tmpd,NNG_OPT_INTERFACE_BW,0))!=0){
                fatal("nng_dialer set INTERFACE_BW", rv);
        }
        if((rv = nng_dialer_set_string(tmpd,NNG_OPT_TCP_BINDTODEVICE,"eth0"))!=0){
                fatal("nng_dialer_set TCP BINDTODEVICE", rv);
        }
        if((rv=nng_dialer_start(tmpd,0))!=0){
                fatal("nng_dialer_start", rv);
        }
        char* check_devicename;
        if((rv=nng_dialer_get_string(tmpd,NNG_OPT_TCP_BINDTODEVICE,&check_devicename))!=0){
                fatal("nng_dialer_get_string", rv);
        }
        printf("the first interface is:%s\n",check_devicename);
/*
        //second dialer
        nng_dialer tmpd1;
        if ((rv = nng_dialer_create(&tmpd1,sock, url)) != 0) {
                fatal("nng_dialer_create", rv);
        }
        if((rv = nng_dialer_set_int(tmpd1,NNG_OPT_INTERFACE_DELAY,0))!=0){
                fatal("nng_dialer set INTERFACE_DELAY", rv);
        }
        if((rv = nng_dialer_set_int(tmpd1,NNG_OPT_INTERFACE_BW,4))!=0){
                fatal("nng_dialer set INTERFACE_BW", rv);
        }
        if((rv = nng_dialer_set_string(tmpd1,NNG_OPT_TCP_BINDTODEVICE,"wlan0"))!=0){
                fatal("nng_dialer_set TCP BINDTODEVICE", rv);
        }
        if((rv=nng_dialer_start(tmpd1,0))!=0){
                fatal("nng_dialer_start", rv);
        }
        if((rv=nng_dialer_get_string(tmpd1,NNG_OPT_TCP_BINDTODEVICE,&check_devicename))!=0){
                fatal("nng_dialer_get_string", rv);
        }
        printf("the second interface is:%s\n",check_devicename);*/
	unsigned count;
	for(count=0;count<num;count++){
		start = nng_clock();

		if ((rv = nng_msg_alloc(&msg, 0)) != 0) {
			fatal("nng_msg_alloc", rv);
		}
		if(count % 2 == 0){
			if ((rv = nng_msg_header_append_u8(msg,NNG_RECVPOLICY_NORMAL)) != 0) {
		                fatal("nng_msg_header_append_u8", rv);
		        }
		}else{
			if ((rv = nng_msg_header_append_u8(msg,NNG_RECVPOLICY_URGENT)) != 0) {
		                fatal("nng_msg_header_append_u8", rv);
		        }
		}
		
		if(count % 3 == 0){
			if ((rv = nng_msg_header_append_u8(msg,NNG_MSG_INTERFACE_DELAY)) != 0) {
		                fatal("nng_msg_header_append_u8", rv);
		        }
		}else if(count %3 == 1){
			if ((rv = nng_msg_header_append_u8(msg,NNG_MSG_INTERFACE_BW)) != 0) {
		                fatal("nng_msg_header_append_u8", rv);
		        }
		}else{
			if ((rv = nng_msg_header_append_u8(msg,NNG_MSG_INTERFACE_RELIABLE)) != 0) {
		                fatal("nng_msg_header_append_u8", rv);
		        }
		}


		if ((rv = nng_msg_append_u32(msg, msec)) != 0) {
			fatal("nng_msg_append_u32", rv);
		}

		printf("client sendmsg\n");
		if ((rv = nng_sendmsg(sock, msg, 0)) != 0) {
			fatal("nng_send", rv);
		}// nng_sendmsg takes the ownership

		printf("client sendmsg finished\n");
		while ((rv = nng_recvmsg(sock, &msg, NNG_FLAG_NONBLOCK)) != 0) {
			nng_msleep(100);
			//fatal("nng_recvmsg", rv);
		}
		end = nng_clock();
		printf("Request took %u milliseconds.\n", (uint32_t)(end - start));
		uint8_t urgent_level = 0;
		uint8_t used_send_policy = 0;
		nng_msg_header_trim_u8(msg,&urgent_level);
		nng_msg_header_trim_u8(msg,&used_send_policy);
		printf("urgent level of the received msg: %u; send policy: %u\n", urgent_level,used_send_policy);
		nng_msg_free(msg);
	}
	
	nng_close(sock);

	return (0);
}

int
main(int argc, char **argv)
{
	int rc;

	if (argc != 4) {
		fprintf(stderr, "Usage: %s <url> <secs the server waits for> <num of msgs to send>\n", argv[0]);
		exit(EXIT_FAILURE);
	}
	rc = client(argv[1], argv[2], argv[3]);
	exit(rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE);
}
