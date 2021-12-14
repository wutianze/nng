//
// Copyright 2020 Staysail Systems, Inc. <info@staysail.tech>
// Copyright 2018 Capitar IT Group BV <info@capitar.com>
//
// This software is supplied under the terms of the MIT License, a
// copy of which should be located in the distribution where this
// file was obtained (LICENSE.txt).  A copy of the license may also be
// found online at https://opensource.org/licenses/MIT.
//

#ifndef NNG_PROTOCOL_MIXCLIENT_H
#define NNG_PROTOCOL_MIXCLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

NNG_DECL int nng_mixclient_open(nng_socket *);

#define NNG_MIXCLIENT_SELF 0x80
#define NNG_MIXCLIENT_PEER 0x81
#define NNG_MIXCLIENT_SELF_NAME "mixclient"
#define NNG_MIXCLIENT_PEER_NAME "mixserver"

#ifdef __cplusplus
}
#endif

#endif // NNG_PROTOCOL_MIXCLIENT_H