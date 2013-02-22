/*
 * Copyright (c) 2009-2013 Open Information Security Foundation
 *
 * \author Victor Julien <victor@inliniac.net> for Emerging Threats Inc.
 */

#ifndef __APP_LAYER_DNS_TCP_H__
#define __APP_LAYER_DNS_TCP_H__

#include "app-layer-protos.h"
#include "app-layer-parser.h"
#include "app-layer-dns-common.h"
#include "flow.h"
#include "queue.h"
#include "util-byte.h"

void RegisterDNSTCPParsers(void);
void DNSTCPParserTests(void);
void DNSTCPParserRegisterTests(void);

#endif /* __APP_LAYER_DNS_TCP_H__ */
