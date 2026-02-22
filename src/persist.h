#ifndef PERSIST_H
#define PERSIST_H

#include <stddef.h>

/* Persist a small null-terminated string (e.g. governor name) to flash.
 * Implementations write to a reserved flash region; callers should ensure
 * the string is reasonably short (<= 64).
 */
int persist_save(const char *name);
int persist_load(char *out, size_t out_len);

/* Persist arbitrary small blob for rp2040_perf params. The implementation
 * stores the blob in the same reserved sector but at a fixed offset so
 * both the governor name and params can coexist. */
int persist_save_rp_params(const void *buf, size_t len);
int persist_load_rp_params(void *out, size_t maxlen);

#endif
