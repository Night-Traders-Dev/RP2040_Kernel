#include "persist.h"
#include <string.h>
#include <stdio.h>
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/bootrom.h"
#include <stdlib.h>

/* Default flash region: last 64KB of a typical 2MB Pico flash
 * WARNING: This region must be reserved for application use. If you
 * change your firmware layout or use a different board, update this
 * offset accordingly.
 */
#ifndef PERSIST_FLASH_OFFSET
#define PERSIST_FLASH_OFFSET 0x1F0000u
#endif

#define PERSIST_SECTOR_SIZE 0x10000u
#define PERSIST_MAGIC 0x47564F47u /* 'GOVG' */

/* rp2040_perf params blob location (within same sector) */
#define RP_PARAMS_OFFSET 0x100u
#define RP_PARAMS_MAGIC  0x52505050u /* 'RPPP' */

struct persist_rec {
    uint32_t magic;
    uint32_t ver;
    char     name[56];
    uint32_t crc;
};

/* Simple XOR CRC for the small record */
static uint32_t simple_crc(const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t crc = 0xA5A5A5A5u;
    for (size_t i = 0; i < len; ++i) crc = (crc << 7) ^ p[i];
    return crc;
}

int persist_save(const char *name)
{
    if (!name) return -1;
    struct persist_rec rec;
    memset(&rec, 0xFF, sizeof(rec));
    rec.magic = PERSIST_MAGIC;
    rec.ver = 1;
    strncpy(rec.name, name, sizeof(rec.name)-1);
    rec.crc = simple_crc(&rec, offsetof(struct persist_rec, crc));

    uint32_t offset = PERSIST_FLASH_OFFSET;

    /* Read existing sector into RAM so we preserve rp_params blob if present */
    const uint8_t *mapped = (const uint8_t *)(0x10000000UL + offset);
    uint8_t *sector = malloc(PERSIST_SECTOR_SIZE);
    if (!sector) return -1;
    memcpy(sector, mapped, PERSIST_SECTOR_SIZE);

    /* copy our record at start of sector */
    memcpy(sector, &rec, sizeof(rec));

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(offset, PERSIST_SECTOR_SIZE);
    flash_range_program(offset, sector, PERSIST_SECTOR_SIZE);
    restore_interrupts(ints);
    free(sector);
    return 0;
}

int persist_load(char *out, size_t out_len)
{
    if (!out || out_len == 0) return -1;
    struct persist_rec rec;
    uint32_t offset = PERSIST_FLASH_OFFSET;
    /* flash memory is memory-mapped at XIP base (0x10000000), but
     * flash_range_program uses flash offsets. We can read via the
     * mapped address: (XIP_BASE + offset). Use bootrom symbol.
     */
    const uint8_t *mapped = (const uint8_t *)(0x10000000UL + offset);
    memcpy(&rec, mapped, sizeof(rec));

    if (rec.magic != PERSIST_MAGIC) return -1;
    uint32_t crc = simple_crc(&rec, offsetof(struct persist_rec, crc));
    if (crc != rec.crc) return -1;

    rec.name[sizeof(rec.name)-1] = '\0';
    strncpy(out, rec.name, out_len-1);
    out[out_len-1] = '\0';
    return 0;
}

int persist_save_rp_params(const void *buf, size_t len)
{
    if (!buf || len == 0) return -1;
    if (len > (PERSIST_SECTOR_SIZE - RP_PARAMS_OFFSET - 16)) return -1;
    uint32_t offset = PERSIST_FLASH_OFFSET;
    const uint8_t *mapped = (const uint8_t *)(0x10000000UL + offset);
    uint8_t *sector = malloc(PERSIST_SECTOR_SIZE);
    if (!sector) return -1;
    memcpy(sector, mapped, PERSIST_SECTOR_SIZE);

    uint32_t p = RP_PARAMS_OFFSET;
    uint32_t magic = RP_PARAMS_MAGIC;
    memcpy(&sector[p], &magic, sizeof(magic)); p += sizeof(magic);
    uint32_t llen = (uint32_t)len;
    memcpy(&sector[p], &llen, sizeof(llen)); p += sizeof(llen);
    memcpy(&sector[p], buf, len); p += len;
    uint32_t crc = simple_crc(buf, len);
    memcpy(&sector[p], &crc, sizeof(crc)); p += sizeof(crc);

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(offset, PERSIST_SECTOR_SIZE);
    flash_range_program(offset, sector, PERSIST_SECTOR_SIZE);
    restore_interrupts(ints);
    free(sector);
    return 0;
}

int persist_load_rp_params(void *out, size_t maxlen)
{
    if (!out || maxlen == 0) return -1;
    uint32_t offset = PERSIST_FLASH_OFFSET;
    const uint8_t *mapped = (const uint8_t *)(0x10000000UL + offset);
    uint32_t p = RP_PARAMS_OFFSET;
    uint32_t magic = 0;
    memcpy(&magic, &mapped[p], sizeof(magic));
    if (magic != RP_PARAMS_MAGIC) return -1;
    p += sizeof(magic);
    uint32_t llen = 0;
    memcpy(&llen, &mapped[p], sizeof(llen));
    p += sizeof(llen);
    if (llen == 0 || llen > maxlen) return -1;
    memcpy(out, &mapped[p], llen);
    p += llen;
    uint32_t crc = 0;
    memcpy(&crc, &mapped[p], sizeof(crc));
    uint32_t calc = simple_crc(out, llen);
    if (crc != calc) return -1;
    return (int)llen;
}
