#pragma once
#include "orb_c_api.h"  // ORB_C_BEGIN/END (impl is C++ now; caller main.c is C)
#include "xbox_one_protocol.h"

ORB_C_BEGIN

int identifiers_get_n();
int identifiers_get_announce(xbox_packet_t *packet);
int identifiers_get(uint8_t sequence, xbox_packet_t *packet);

ORB_C_END
