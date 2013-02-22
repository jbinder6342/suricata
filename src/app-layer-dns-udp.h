/*
 * Copyright (c) 2009-2013 Open Information Security Foundation
 *
 * \author Victor Julien <victor@inliniac.net> for Emerging Threats Inc.
 */

#ifndef __APP_LAYER_DNS_UDP_H__
#define __APP_LAYER_DNS_UDP_H__

#include "app-layer-protos.h"
#include "app-layer-parser.h"
#include "app-layer-dns-common.h"
#include "flow.h"
#include "queue.h"
#include "util-byte.h"

void RegisterDNSUDPParsers(void);
void DNSUDPParserTests(void);
void DNSUDPParserRegisterTests(void);

#endif /* __APP_LAYER_DNS_UDP_H__ */
