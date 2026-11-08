#ifndef BEER_WIN32_SDDL_H
#define BEER_WIN32_SDDL_H

#include <stddef.h>
#include <stdint.h>

/* Security Descriptor Definition Language (SDDL) -> self-relative SECURITY_DESCRIPTOR, as
 * ConvertStringSecurityDescriptorToSecurityDescriptor builds it.
 *
 * Supported: O:/G: owner and group SIDs; D:/S: DACL and SACL with the P, AI, AR flags (and NO_ACCESS_CONTROL for
 * a NULL DACL); ACE types A, D, AU, AL, ML; ACE flags CI, OI, NP, IO, ID, SA, FA; rights as hex/decimal numbers or
 * the generic, standard, file, registry, directory-service and mandatory-label abbreviations; SIDs as the
 * well-known two-letter aliases or literal S-1-... strings. Object ACEs (OA, OD, ...) and domain-relative aliases
 * (DA, DU, ...) are rejected, as on a machine outside a domain.
 *
 * The input is UTF-16 (wide) or ASCII (narrow == 1). On success returns 0, writes the descriptor to out when out
 * is non-NULL and capacity is large enough, and stores the required size in *size. Returns a Win32 error code
 * (87 ERROR_INVALID_PARAMETER for malformed input, 122 ERROR_INSUFFICIENT_BUFFER when capacity is too small). */
uint32_t beer_sddl_to_descriptor(const void *sddl, int narrow, uint8_t *out, size_t capacity, size_t *size);

#endif
