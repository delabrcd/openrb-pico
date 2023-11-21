#ifndef OPENRB_IDENTIFIERS_H
#define OPENRB_IDENTIFIERS_H
#include "xbox_one_protocol.h"

int identifiers_get_n();
int identifiers_get_announce(xbox_packet_t *packet);
int identifiers_get(uint8_t sequence, xbox_packet_t *packet);

#endif  // OPENRB_IDENTIFIERS_H