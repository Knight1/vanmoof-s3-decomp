#include "batteryware.h"

/*
 * Batteryware main entry point — startup sequence after Reset_Handler.
 *
 * Configures NVIC, RCC, SRAM parity, VTOR redirect, then chains
 * through peripheral_init, fuel gauge init, DMA init, flash config,
 * and system_init before handing off to the main super-loop.
 */
void batteryware_main(void)
{
    /* RCC base — clock control registers */
    volatile uint32_t * const RCC    = (volatile uint32_t *)0x40021000;
    /* NVIC/syscfg scratch */
    volatile uint32_t *       s_cfg  = (volatile uint32_t *)0x20002C60;
    /* USART/baud scratch */
    volatile uint32_t *       s_regs = (volatile uint32_t *)0x20002C64;

    nvic_enable_irq_s_dsb(27);

    RCC[0x34 / 4] &= 0xFFFFFDFF;       /* clear bit 9 in AHB2ENR? */

    s_cfg[0x14 / 4] = 1;               /* SRAM parity enable */

    nvic_enable_irq_s_dsb(5);

    extern void disable_irqs(void);
    disable_irqs();

    extern void set_vector_table(uint32_t, uint32_t, uint32_t);
    set_vector_table(0x20000000, 0x020000C0, 0xC0);   /* VTOR → SRAM */

    s_regs[2] = 0x20000000;            /* baud rate/scratch */

    __DSB();
    extern void enable_irqs(void);
    enable_irqs();

    bool reset_done = peripheral_reset();

    *s_regs = (*s_regs & 0xFFFFEFFF) | 0x1000;

    RCC[0x34 / 4] |= 1;               /* IOPAEN */
    RCC[0x38 / 4] |= 0x10000000;      /* USART2EN? */

    peripheral_init(reset_done);
    extern void fg_init(void);
    fg_init();
    dma_init();

    volatile uint32_t *s_flash_cfg = (volatile uint32_t *)0x20002C68;
    *s_flash_cfg = 0x00000005;

    flash_enable_prefetch();
    extern void flash_unlock(void);
    flash_unlock();
    system_init();
}

/*
 * Peripheral init — 3-phase USART/DMA/GPIO startup.
 *
 * Phase 1: zero 3 local structs, set RCC bit 0x800,
 *   fill DMA/bus-fault params after memset, then call bus_fault_reset.
 *   system_reset on failure.
 *
 * Phase 2: fill USART-clock-struct (0xF prescaler, flag at +4=1,
 *   USART-enable at +8=0x80), call usart_bus_config. reset on failure.
 *
 * Phase 3: configure RCC struct, call rcc_reconfigure, reset on failure.
 */
void peripheral_init(bool arg)
{
    uint32_t dma_cfg[14];       /* 0x38 bytes — DMA/bus config */
    uint8_t  usart_cfg[20];     /* 0x14 bytes — USART clock config */
    uint32_t rcc_cfg[9];        /* 0x24 bytes — RCC config */

    (void)arg;

    memset_byte_fill((uint8_t *)dma_cfg,   0, 0x38);
    memset_byte_fill(usart_cfg,            0, 0x14);
    memset_byte_fill((uint8_t *)rcc_cfg,   0, 0x24);

    volatile uint32_t *s_rcc = (volatile uint32_t *)0x20002C6C;
    *s_rcc = (*s_rcc & 0xFFFFFFF7) | 0x800;

    dma_cfg[0] = 10;
    *(uint32_t *)((uint8_t *)dma_cfg + 0x30) = 1;     /* bit-banding / flags */
    *(uint32_t *)((uint8_t *)dma_cfg + 0x2C) = 0x10;  /* prescaler / period */
    *(uint32_t *)((uint8_t *)dma_cfg + 0x28) = 1;     /* enable */
    *(uint32_t *)((uint8_t *)dma_cfg + 0x14) = 1;     /* direction / mode */

    extern int bus_fault_reset(void *);
    if (bus_fault_reset(dma_cfg) != 0) {
        system_reset();
    }

    usart_cfg[0] = 0x0F;        /* prescaler */
    usart_cfg[4] = 1;           /* enable flag */
    usart_cfg[8] = 0x80;        /* USART enable */
    /* bytes 1-3,5-7,9-19 stay 0 from memset */

    extern int usart_bus_config(void *, uint8_t);
    if (usart_bus_config(usart_cfg, 0) != 0) {
        system_reset();
    }

    rcc_cfg[0] = 1;
    rcc_cfg[1] = 2;

    if (rcc_reconfigure(rcc_cfg) != 0) {
        system_reset();
    }
}

/*
 * Main super-loop — BMS state dispatch.
 *
 * Boot sequence: bms_setup → print banner → check UVP/OVP startup
 * state vs button GPIO → dispatch via voltage comparison to state
 * handlers → enter infinite loop polling flags and dispatching
 * state callbacks via jump table at 0x08005B30.
 *
 * SRAM layout used:
 *   0x20002B58  g_bms_state        current BMS state (0–0x19)
 *   0x20002C00  g_bms_timer        state timer counter
 *   0x20002C44  g_fault_flags      central fault register
 *   0x20002C48  g_fault_shadow     fault shadow/acknowledge
 *   0x20002C50  g_fault_pending    pending fault bits
 *   0x20002C54  g_state_flags      state engine flag bits
 *   0x20002C58  g_cell_status      cell voltage base struct
 *   0x20002C5C  g_state_cfg        state config bits
 *   0x20002C60  g_cmp1             voltage comparator 1 (uint16)
 *   0x20002C64  g_cmp2             voltage comparator 2 (uint16)
 */
void main_loop(void)
{
    extern void bms_setup(void);

    batteryware_main();
    bms_setup();

    volatile uint32_t * const g_timer        = (volatile uint32_t *)0x20002C00;
    volatile uint32_t * const g_flags         = (volatile uint32_t *)0x20002C44;
    volatile uint32_t * const g_state        = (volatile uint32_t *)0x20002B58;
    volatile uint8_t  * const g_bms_state    = (volatile uint8_t  *)0x20002B58;

    *g_timer = 0;

    /* Print boot banner */
    uart_puts("I am VanMoof AP\r\n");
    uart_tx_flush();

    /* Load pending faults */
    *g_flags = *(volatile uint32_t *)0x20002C50;

    /* Startup state check: UVP/OVP power-on vs normal boot */
    if (*g_bms_state == 0x17 || *g_bms_state == 0x18) {
        /* UVP/OVP power-on path */
        if (*g_bms_state == 0x17) {
            volatile uint32_t *g_fault_shadow = (volatile uint32_t *)0x20002C48;
            *g_fault_shadow |= 0x40;     /* mark UVP detected */
        }
        volatile uint32_t *g_state_flags = (volatile uint32_t *)0x20002C54;
        *g_state_flags |= 0x8000;
        state_handler_17_19();

        uart_puts("Power On Detect Mode\r\n");
    } else {
        /* Normal boot — check button GPIO */
        bool button_pressed = gpio_bit_read(0x50000000, 0x800);
        volatile uint32_t *g_state_flags = (volatile uint32_t *)0x20002C54;

        if (button_pressed) {
            *g_state_flags |= 8;
            uart_puts("DP Mode\r\n");
        } else {
            *g_state_flags &= ~8U;
            uart_puts("VanMoof Mode\r\n");
        }

        extern void veneer_11f48(void);
        veneer_11f48();

        /* Dispatch to charge/discharge states based on voltage comparators */
        volatile uint32_t *g_cell_status = (volatile uint32_t *)0x20002C58;
        volatile uint32_t *g_state_cfg   = (volatile uint32_t *)0x20002C5C;
        volatile uint16_t *g_cmp1        = (volatile uint16_t *)0x20002C60;
        volatile uint16_t *g_cmp2        = (volatile uint16_t *)0x20002C64;

        if (*g_bms_state == 10 && *g_cmp1 < *(volatile uint16_t *)((uint8_t *)g_cell_status + 0x46)) {
            uart_puts("FEDL5236_Max_Cell_Voltage over threshold\r\n");
            *g_state = 10;
            *g_state_cfg |= 8;
            extern void state_handler_0a(void);
            state_handler_0a();
        } else if (*g_bms_state == 9 && *g_cmp1 < *(volatile uint16_t *)((uint8_t *)g_cell_status + 0x3E)) {
            uart_puts("FEDL5236_Min_Cell_Voltage under threshold\r\n");
            *g_state = 9;
            *g_state_cfg |= 4;
            extern void state_handler_09(void);
            state_handler_09();
        } else if (*g_bms_state == 8 && *(volatile uint16_t *)((uint8_t *)g_cell_status + 0x36) < *g_cmp2) {
            uart_puts("CHG CAL over threshold\r\n");
            *g_state = 8;
            *g_state_cfg |= 2;
            extern void state_handler_08(void);
            state_handler_08();
        } else if (*g_bms_state == 7 && *(volatile uint16_t *)((uint8_t *)g_cell_status + 0x2E) < *g_cmp2) {
            uart_puts("DSG CAL over threshold\r\n");
            *g_state = 7;
            *g_state_cfg |= 1;
            extern void state_handler_07(void);
            state_handler_07();
        } else {
            state_handler_01();
        }
    }

    uart_tx_flush();

    /* Infinite state dispatch loop via jump table at 0x08005B30 */
    void (* const * const g_state_jump_table)(void) =
        (void (* const * const)(void))0x08005B30;

    while (1) {
        /* Check if any fault flag is active; if not, dispatch state timer */
        if (((*g_flags >> 1) & 1) == 0 &&
            ((*g_flags & 1) == 0) &&
            ((*g_flags >> 2) & 1) == 0) {
            if (*g_state < 0x1A) {
                g_state_jump_table[*g_state]();
                return;
            }
            state_handler_01();
        }
        extern void uart_resp_handler(void);
        uart_resp_handler();
        uart_tx_isr();
    }
}