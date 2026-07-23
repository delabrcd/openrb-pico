#pragma once
#include "xbox_one_protocol.h"

// Plain C++ free functions (every consumer is a C++ TU); defined in wla_identifiers.cpp.
int identifiers_get_n();
int identifiers_get_announce(XboxPacket *packet);
int identifiers_get(uint8_t sequence, XboxPacket *packet);
