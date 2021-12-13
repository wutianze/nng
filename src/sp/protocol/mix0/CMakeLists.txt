#
# Copyright 2019 Staysail Systems, Inc. <info@staystail.tech>
# Copyright 2018 Capitar IT Group BV <info@capitar.com>
#
# This software is supplied under the terms of the MIT License, a
# copy of which should be located in the distribution where this
# file was obtained (LICENSE.txt).  A copy of the license may also be
# found online at https://opensource.org/licenses/MIT.
#

#  MIXv0 protocol
nng_directory(mix0)

nng_sources_if(NNG_PROTO_MIXSERVER0 mixserver.c)
nng_headers_if(NNG_PROTO_MIXSERVER0 nng/protocol/mix0/mixserver.h)
nng_defines_if(NNG_PROTO_MIXSERVER0 NNG_HAVE_MIXSERVER0)

nng_sources_if(NNG_PROTO_MIXCLIENT0 mixclient.c)
nng_headers_if(NNG_PROTO_MIXCLIENT0 nng/protocol/mix0/mixclient.h)
nng_defines_if(NNG_PROTO_MIXCLIENT0 NNG_HAVE_MIXCLIENT0)


