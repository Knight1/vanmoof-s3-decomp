#include <stdint.h>

#include "bim.h"
#include "main.h"

/* MMIO config word; low 4 bits select an OAD chunk size (the value
 * gets pre-shifted by 10, so the selectable sizes are
 * {0, 1024, 2048, ..., 15360}). Downstream the BIM's CRC32 routine
 * uses this as the outer-loop chunk stride: it partitions the
 * image into chunks of this size and CRCs each one. Probably keyed
 * off a hardware-revision pad on the BLE PCB so different board
 * revisions can use different flash access patterns.
 *
 * Address verified from the literal pool of FUN_00057000 at file
 * offset 0x1018 of bleboot_1.0.0.bin; the exact register is not
 * yet identified in the CC2642R1F TRM (sits in the
 * 0x40030000..0x40034000 band, between the FLASH controller and
 * VIMS). The name "hw_id" used during the first decomp pass was
 * misleading — it's a chunk-size selector, not an identity. */
#define BIM_CHUNK_SIZE_REG   (*(volatile uint32_t *)0x40032430u)

/* SRAM global at 0x20000400 — caches the (low_4_bits << 10) chunk
 * size so downstream CRC compute doesn't re-read the MMIO each
 * call. Lives 256 bytes after `bim_crc32_image`'s scratch buffer
 * at 0x20000300; the two globals are adjacent in the BIM's SRAM
 * layout. */
volatile uint32_t g_oad_chunk_size;

int main(void)
{
    g_oad_chunk_size = (BIM_CHUNK_SIZE_REG & 0xFu) << 10;
    bim_dispatch();
    return 0;
}
