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

nng_sources_if(NNG_PROTO_MIX0 mix.c)
nng_headers_if(NNG_PROTO_MIX0 nng/protocol/mix0/mix.h)
nng_defines_if(NNG_PROTO_MIX0 NNG_HAVE_MIX0)

#nng_test(mix0_test)