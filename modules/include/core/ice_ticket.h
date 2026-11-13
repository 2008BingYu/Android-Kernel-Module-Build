#ifndef ICE_TICKET_H
#define ICE_TICKET_H

int ice_ticket_verify(const char *ticket, int len);

int ice_ed25519_verify(const unsigned char *sig, const unsigned char *pk,
		       const unsigned char *msg, unsigned long long msglen);

#endif
