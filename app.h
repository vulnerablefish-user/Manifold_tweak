/* SPDX-License-Identifier: MPL-2.0 */
/* MANIFOLD: the app's half of the fm1-base contract (device/unity.c, host/plat_host.c). */
#pragma once
#include <stdint.h>
#include "plat.h"

#define APP_NAME "MANIFOLD"
#define APP_SPLASH_RGB RGB(120, 190, 255)
#define APP_BLOCK 256u                   /* frames per audio block: 5.8 ms at 44.1 kHz (four 64-sample control ticks); the I2S half buffer too, so an interrupt held up a millisecond or two by the radio or USB is not a dropout */
#define APP_TOUCH                        /* velocity and pressure from the keys (lib/touch.h) */
#define APP_BLE_AT_BOOT (manifold_bt_on())   /* the radio at power-on: as the player left it (Setup) */
int manifold_bt_on(void);
#define APP_TICK                         /* MIDI out (1.0.6): the clock, the drums, the layers' notes (midi_out.h) */
#define APP_OVERLOAD                     /* the platform tells us when a render ran past 90 % (counted, shown) */
void app_overload(void);

/* the store: settings (with the profiler's cuts), the panel calibration (the platform's), 64 patch slots in four banks
 * of 16 (each with its four scenes), Bluetooth's. An object's number is written in its record: the 16 slots of 1.0 and
 * Bluetooth's keep theirs, the 48 slots from 1.0.4 come after. */
#define N_BANK 4
#define N_PATCH 64
enum { OBJ_SETTINGS, OBJ_PANEL,
       OBJ_P0, OBJ_P1, OBJ_P2, OBJ_P3, OBJ_P4, OBJ_P5, OBJ_P6, OBJ_P7,
       OBJ_P8, OBJ_P9, OBJ_P10, OBJ_P11, OBJ_P12, OBJ_P13, OBJ_P14, OBJ_P15,
       OBJ_BLE,                          /* Bluetooth: the radio's address and the audio devices' keys */
       OBJ_P16,                          /* slots 17..64 (banks B..D): OBJ_P16 + slot - 16 */
       OBJ_NOBJ = OBJ_P16 + N_PATCH - 16 };
/* The pool (fm1-base storage.c): the 64 slots and Bluetooth's share the 69 sectors 0x97000..0xDBFFF (the panel's pair
 * at 0xDC000 after it), one copy per sector, instead of a sector pair each (34 pairs would not fit). 1.0's 16 slot
 * pairs, at the pool's first 32 sectors, are pool copies as they are; Bluetooth's pair at 0xDE000 (sectors 71..72 from
 * the base: "OLD" 73) moves in at the first boot. 65 objects in 69 sectors: four to spare, a save always finds one. */
#define APP_STORE_POOL
#define APP_STORE_POOL_BASE 0x97000u
#define APP_STORE_POOL_N 69u
#define APP_STORE_POOL_OLD 73u
#define APP_STORE_POOL_OBJ0 OBJ_P0
#define APP_STORE_POOL_OBJN (OBJ_NOBJ - OBJ_P0)
/* a pooled copy that is not MANIFOLD's (another firmware's record left in the data area, the same record format): a
 * patch slot's current copy whose payload is no MANIFOLD patch ("MNP8", "MNP9", "MNPA") is erased at boot, so it can
 * never stand in front of one (storage.c st_pool_boot) */
#define APP_STORE_POOL_FOREIGN(obj, p, n) ((obj) != OBJ_BLE && !manifold_patch_magic((const unsigned char *)(p), (n)))
static inline int manifold_patch_magic(const unsigned char *p, unsigned n)
{
    return n >= 4u && p[1] == 0x50u && p[2] == 0x4Eu && p[3] == 0x4Du && (p[0] == 0x38u || p[0] == 0x39u || p[0] == 0x41u || p[0] == 0x42u || p[0] == 0x43u || p[0] == 0x44u || p[0] == 0x45u || p[0] == 0x46u || p[0] == 0x47u || p[0] == 0x48u);
}
/* Bluetooth audio (fm1-base's own stack): the radio's receive queue, between its interrupt and the main loop. A packet
   acknowledged on air and dropped here when the queue is full is lost for good, and the audio set-up stalls; fm1-base
   sets 12 for light firmwares. MANIFOLD's main loop is slower (screen updates under a busy audio render): 48, about
   17 KB of the pool (13 KB more than 12) */
#define BTI_RING 48u
#define APP_STORE_MAP {0xFC000u, 0xDC000u, \
    0x97000u, 0x99000u, 0x9B000u, 0x9D000u, 0x9F000u, 0xA1000u, 0xA3000u, 0xA5000u, \
    0xA7000u, 0xA9000u, 0xAB000u, 0xAD000u, 0xAF000u, 0xB1000u, 0xB3000u, 0xB5000u, \
    0xDE000u}

/* the contract: the platform calls these */
void app_init(void);
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12);
void app_panic(void);
void app_ui_init(void);
void app_frame(void);
void app_input_only(void);
void app_say(const char *a, const char *b);

#define APP_HOST_EXPECT
int app_expect(const char *what, int *got);
