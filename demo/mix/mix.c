#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <nng/nng.h>
#include <nng/protocol/mix0/mix.h>

#define NODE0 "node0"
#define NODE1 "node1"

void
fatal(const char *func, int rv)
{
        fprintf(stderr, "%s: %s\n", func, nng_strerror(rv));
        exit(1);
}

int
send_name(nng_socket sock, char *name)
{
        int rv;
        printf("%s: SENDING \"%s\"\n", name, name);
        if ((rv = nng_send(sock, name, strlen(name) + 1, 0)) != 0) {
                fatal("nng_send", rv);
        }
        return (rv);
}

int
recv_name(nng_socket sock, char *name)
{
        char *buf = NULL;
        int rv;
        size_t sz;
        if ((rv = nng_recv(sock, &buf, &sz, NNG_FLAG_ALLOC)) == 0) {
                printf("%s: RECEIVED \"%s\"\n", name, buf); 
                nng_free(buf, sz);
        }else{
                printf("RECEIVE error\n");
        }
        return (rv);
}

int
recv_send(nng_socket sock, char *name)
{
        int rv;
        /*if ((rv = nng_socket_set_ms(sock, NNG_OPT_RECVTIMEO, 100)) != 0) {
                fatal("nng_setopt_ms", rv);
        }*/
        for (;;) {
                recv_name(sock, name);
                sleep(1);
                send_name(sock, name);
        }
}

int
send_recv(nng_socket sock, char *name)
{
        int rv;
        /*if ((rv = nng_socket_set_ms(sock, NNG_OPT_RECVTIMEO, 100)) != 0) {
                fatal("nng_setopt_ms", rv);
        }*/
        for (;;) {
                send_name(sock, name);
                sleep(1);
                recv_name(sock, name);
        }
}

int
node0(const char *url)
{
        nng_socket sock;
        int rv;
        if ((rv = nng_pair0_open(&sock)) != 0) {
                fatal("nng_pair0_open", rv);
        }
         if ((rv = nng_listen(sock, url, NULL, 0)) !=0) {
                fatal("nng_listen", rv);
        }
        return (recv_send(sock, NODE0));
}

int
node1(const char *url)
{
        nng_socket sock;
        int rv;
        sleep(1);
        if ((rv = nng_mix_open(&sock)) != 0) {
                fatal("nng_mix_open", rv);
        }

        //first dialer
        nng_dialer tmpd;//only an id, dont worry to pass by value
        if ((rv = nng_dialer_create(&tmpd,sock, url)) != 0) {
                fatal("nng_dialer_create", rv);
        }
        if((rv = nng_dialer_setopt_int(tmpd,NNG_OPT_INTERFACE_DELAY,5))!=0){
                fatal("nng_dialer set INTERFACE_DELAY", rv);
        }
        if((rv = nng_dialer_setopt_int(tmpd,NNG_OPT_INTERFACE_BW,0))!=0){
                fatal("nng_dialer set INTERFACE_BW", rv);
        }
        if((rv = nng_dialer_setopt_string(tmpd,NNG_OPT_TCP_BINDTODEVICE,"wlan0"))!=0){
                fatal("nng_dialer_set TCP BINDTODEVICE", rv);
        }
        if((rv=nng_dialer_start(tmpd,0))!=0){
                fatal("nng_dialer_start", rv);
        }
        char* check_devicename;
        if((rv=nng_dialer_getopt_string(tmpd,NNG_OPT_TCP_BINDTODEVICE,&check_devicename))!=0){
                fatal("nng_dialer_getopt_string", rv);
        }
        printf("the first interface is:%s\n",check_devicename);

        //second dialer
        nng_dialer tmpd1;
        if ((rv = nng_dialer_create(&tmpd1,sock, url)) != 0) {
                fatal("nng_dialer_create", rv);
        }
        if((rv = nng_dialer_setopt_int(tmpd1,NNG_OPT_INTERFACE_DELAY,0))!=0){
                fatal("nng_dialer set INTERFACE_DELAY", rv);
        }
        if((rv = nng_dialer_setopt_int(tmpd1,NNG_OPT_INTERFACE_BW,5))!=0){
                fatal("nng_dialer set INTERFACE_BW", rv);
        }
        if((rv = nng_dialer_setopt_string(tmpd1,NNG_OPT_TCP_BINDTODEVICE,"wlan0"))!=0){
                fatal("nng_dialer_set TCP BINDTODEVICE", rv);
        }
        if((rv=nng_dialer_start(tmpd1,0))!=0){
                fatal("nng_dialer_start", rv);
        }
        if((rv=nng_dialer_getopt_string(tmpd1,NNG_OPT_TCP_BINDTODEVICE,&check_devicename))!=0){
                fatal("nng_dialer_getopt_string", rv);
        }
        printf("the second interface is:%s\n",check_devicename);
        
        return (send_recv(sock, NODE1));
}

int
main(int argc, char **argv)
{
        if ((argc > 1) && (strcmp(NODE0, argv[1]) == 0))
                return (node0(argv[2]));

        if ((argc > 1) && (strcmp(NODE1, argv[1]) == 0))
                return (node1(argv[2]));

        fprintf(stderr, "Usage: mix %s|%s <URL> <ARG> ...\n", NODE0, NODE1);
        return 1;
}
