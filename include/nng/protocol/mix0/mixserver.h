//
// Copyright 2020 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2018 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#ifndef NNG_PROTOCOL_MIXSERVER_H
#define NNG_PROTOCOL_MIXSERVER_H

#ifdef __cplusplus
extern "C" {
#endif

NNG_DECL int nng_mixserver_open(nng_socket *);

#define NNG_MIXSERVER_SELF 0x81
#define NNG_MIXSERVER_PEER 0x80
#define NNG_MIXSERVER_SELF_NAME "mixserver"
#define NNG_MIXSERVER_PEER_NAME "mixclient"

#ifdef __cplusplus
}
#endif

#endif // NNG_PROTOCOL_MIXSERVER_H
