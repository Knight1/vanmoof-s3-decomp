/*
 * battery.c — VanMoof S3 mainware battery / BMS Modbus driver.
 *
 * Modbus-RTU master to the battery BMS (slave 0xAA) over the inter-module bus:
 * PDU builders (func 3 read / func 6 write), the per-call transaction pump and
 * the byte-level master state machine (request + CRC-16 / response + verify),
 * the telemetry-register unpacker (BMS registers -> the app-context cache), the
 * super-loop service step, and the battery presence/charge/error state machine.
 *
 * Behaviour-equivalent reconstruction of the live disassembly (exact control
 * flow, ctx/frame offsets + widths, Modbus framing, and log strings). The
 * register map cross-validates the batteryware decomp (slave 0xAA, func 3/6).
 *
 * Memory model (resolved from the OEM literal pool):
 *   g_bat_modbus_ctx  0x20006E90  Modbus context: master-SM state [0], RX
 *                                 counter [2], bus handle [0xE74], txn-SM state
 *                                 [0xE78], request/response frame [0xE7C], func
 *                                 [0xE7D].
 *   g_bat_frame       0x20007D0C  == &g_bat_modbus_ctx[0xE7C] (the Modbus PDU).
 *   G_APP_CTX         0x200083A8  telemetry destination (reg N -> +0x3F2+N*2).
 */

#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#include "app.h"        /* state_flag_set, maybe_get_bike_state */
#include "log.h"        /* g_log_func, log_print_timestamp_prefix */
#include "scheduler.h"  /* scheduler_alloc/release/start/slot_is_idle/set_timer_name */
#include "systick.h"    /* systick_now, systick_delay */
#include "watchdog.h"   /* watchdog_kick */

/* app/session context (telemetry destination) */
#define G_APP_CTX  ((uint8_t *)0x200083A8u)

/* the log/IO vtable at 0x20009D98: [0] is g_log_func (the logger); [1] is the
 * byte sink the bus RX path streams received bytes through. */
typedef void (*io_fn_t)();
extern int pack_validate();
unsigned int bat_modbus_master_step(uint8_t *frame);
void bms_telemetry_unpack(uint8_t *frame);
void battery_telemetry_step(int param_1);
void battery_state_process(int param_1, int param_2);
void battery_charge_display_step(int param_1);

/* ── battery driver state ───────────────────────────────────────────────────*/
extern uint8_t g_bat_modbus_ctx[];                 /* 0x20006E90 */
extern const char *const g_power_state_name_table[]; /* power-state name strings */

/* ── shared bus transport + app cross-refs + leaf helpers (K&R: the 7 source
 *    groups call these with varied arg styles; K&R sidesteps signature clashes,
 *    and these all gc-section out anyway). ────────────────────────────────────*/
extern int HAL_GPIO_ReadPin();
extern int HAL_GPIO_WritePin();
extern int bus_rx_byte_locked();
extern int bus_crc16_get();
extern int bus_crc16_update();
extern int bus_crc16_reset();
extern int bus_crc16_verify();
extern int bus_tx_enqueue_byte();
extern int bus_tx_enqueue_n();
extern int state_flags_set();
extern int state_flags_clear();
extern int power_state_get_clamped();
extern int maybe_enqueue_tx_message();
extern int sched_timer_arm_or_alloc();
extern int motor_fw_update_fsm_step();
extern int console_cmd_battery();
extern int bus_queue_push();   /* bus ring enqueue PDU */
extern int bus_queue_pop();   /* bus ring peek/has-frame */
extern int bus_queue_reset();   /* bus ring reset */
extern int shift_aux_state_reset();
extern uint32_t bus_queue_peek(void *q, uint16_t *out);  /* shifter.c (0x08037f34) */
/* scheduler_slot_get_remaining: declared in scheduler.h.
 * status_clear_pulse_flag, bus_queue_peek_status, battery_charge_lookup,
 * battery_charge_complete_watchdog, battery_set_charge_mode,
 * battery_telemetry_state_get and battery_fault_warning_report are DEFINED below. */

/* ───────────────────────────────────────────────────────────────────────── */
/*
 * VanMoof S3 mainware (STM32F413) - battery/BMS Modbus driver (R1-pdu+small)
 * Modbus RTU master -> BMS slave 0xAA over the inter-module bus.
 *
 * Shared memory model:
 *   g_bat_modbus_ctx : base 0x20006E90 (extern uint8_t[])
 *       [0xE74] = bus/ring-buffer handle pointer (uint32_t)
 *   g_bat_frame      : &g_bat_modbus_ctx[0xE7C] == 0x20007D0C (the Modbus PDU frame)
 *   g_log_func       : (*(code*)*DAT_0x20009D98)(fmt, ...)  (log.h)
 *
 * State bytes for the battery state machine live at 0x200000E7 (g_bat_state, a
 * scheduler-slot uint8_t region: this driver reads/writes the substate at +3).
 * 0x20008A00 = batteryware-update record (g_batware_update).
 */


/* ---- shared externs (see model) ---- */
#define g_bat_frame   (g_bat_modbus_ctx + 0xE7C)   /* 0x20007D0C */


/* ring-buffer helpers used by the bus handle (kept FUN_ named, extern'd) */

/* HAL */

/* battery state region (g_bat_state @ 0x200000E7); update record @ 0x20008A00 */
#define G_BAT_STATE     ((uint8_t *)0x200000E7u)
#define G_BATWARE_UPD   ((uint8_t *)0x20008A00u)
#define G_STATUS_CLK    ((uint8_t *)0x200001D8u)   /* status/clock block (states.c G_CLK) */

#define GPIOC_BASE      0x40020800u

/* forward decls of this driver's own members */
int  modbus_bat_submit(void *frame);
void bms_modbus_read(uint16_t reg, uint8_t count);
void bms_modbus_write(uint16_t reg, uint32_t value);

/* charge-policy + fault/status leaf helpers (defined at end of file) */
int      battery_charge_lookup(uint32_t soc, uint16_t flags, int16_t temp_delta,
                               uint16_t *out_rate, char *out_mode);
void     battery_charge_complete_watchdog(int target_rate, uint16_t *out_rate);
void     battery_set_charge_mode(uint8_t mode);
uint8_t  battery_telemetry_state_get(void);
void     battery_fault_warning_report(int ctx);
uint16_t bus_queue_peek_status(void);
void     status_clear_pulse_flag(void);


/*
 * modbus_bat_submit @ 0x08039ddc
 * Enqueue a built Modbus PDU into the BMS bus ring buffer.
 * Returns 1 if the enqueue helper returned non-zero (busy/error), else 0.
 */
int modbus_bat_submit(void *frame)
{
    int iVar1;

    iVar1 = (int)bus_queue_push(*(void **)(g_bat_modbus_ctx + 0xE74), frame);
    return (iVar1 != 0);
}


/*
 * bms_modbus_read @ 0x0803e5fc
 * Build a function-3 (read holding registers) PDU to slave 0xAA and submit it.
 *   frame[0]    = 0xAA  (slave)
 *   frame[1]    = 0x03  (func: read)
 *   frame[2..3] = reg   (u16, native store)
 *   frame[0x84] = count (uint8_t/expected-count field)
 * On submit failure, log "BMS MB Read Error\r\n".
 */
void bms_modbus_read(uint16_t reg, uint8_t count)
{
    uint8_t  frame[0x110];   /* mirrors local_110 .. local_8c stack frame */

    frame[0x00] = 0xAA;                 /* local_110 : slave  */
    frame[0x01] = 0x03;                 /* local_10f : func   */
    *(uint16_t *)(frame + 0x02) = reg;  /* local_10e : reg    */
    frame[0x84] = count;                /* local_8c  : count  */

    if (modbus_bat_submit(frame) != 0) {
        g_log_func("BMS MB Read Error\r\n");
    }
}


/*
 * bms_modbus_write @ 0x0803e634
 * Build a function-6 (write single register) PDU to slave 0xAA and submit it.
 *   frame[0]    = 0xAA  (slave)
 *   frame[1]    = 0x06  (func: write)
 *   frame[2..3] = reg   (u16, native store)
 *   frame[4]    = value & 0xFF        (data low)
 *   frame[5]    = (value >> 8) & 0xFF (data high)
 *   frame[0x84] = 1     (uint8_t/expected-count field)
 * On submit failure, log "BMS MB Write Error\r\n".
 */
void bms_modbus_write(uint16_t reg, uint32_t value)
{
    uint8_t  frame[0x110];   /* mirrors local_110 .. local_8c stack frame */

    frame[0x00] = 0xAA;                                 /* local_110 : slave   */
    frame[0x01] = 0x06;                                 /* local_10f : func    */
    frame[0x84] = 1;                                    /* local_8c  : count   */
    frame[0x04] = (uint8_t)value;                       /* local_10c : data lo */
    frame[0x05] = (uint8_t)((value >> 8) & 0xFF);       /* local_10b : data hi */
    *(uint16_t *)(frame + 0x02) = reg;                  /* local_10e : reg     */

    if (modbus_bat_submit(frame) != 0) {
        g_log_func("BMS MB Write Error\r\n");
    }
}


/*
 * bms_write_reg8_and_poll @ 0x0803f45c
 * Write value 1 to BMS register 8, then immediately read register 8 back
 * (poll/confirm). The trailing args in the decompile are calling-convention
 * artifacts; the real callees take (reg,value) and (reg,count).
 */
void bms_write_reg8_and_poll(void)
{
    bms_modbus_write(8, 1);
    bms_modbus_read(8, 1);
}


/*
 * battery_request_telemetry @ 0x0803ea74
 * Kick off the BMS telemetry burst: read 0x31 registers starting at reg 0,
 * then read 0x10 registers starting at reg 0x47.
 */
void battery_request_telemetry(void)
{
    bms_modbus_read(0x00, 0x31);
    bms_modbus_read(0x47, 0x10);
}


/*
 * bat_modbus_reinit @ 0x08039e18
 * Reset the BMS bus ring buffer (head/tail/count). Returns true on success
 * (helper returned 0).
 */
_Bool bat_modbus_reinit(void)
{
    int iVar1;

    iVar1 = (int)bus_queue_reset(*(void **)(g_bat_modbus_ctx + 0xE74));
    return iVar1 == 0;
}


/*
 * battery_on_detect_step @ 0x0803f3e0
 * When the battery substate is 5 (or a force flag is set) and the battery
 * presence GPIO (GPIOC pin 0x400 == PC10) reads high, log "Battery on\r\n"
 * and advance the substate to 1 (battery detected/on).
 */
void battery_on_detect_step(int force)
{
    int iVar1;

    if (((G_BAT_STATE[3] == 0x05) || (force != 0)) &&
        (iVar1 = HAL_GPIO_ReadPin(GPIOC_BASE, 0x400), iVar1 != 0)) {
        log_print_timestamp_prefix();
        g_log_func("Battery on\r\n");
        G_BAT_STATE[3] = 1;
    }
}


/*
 * battery_substate_advance @ 0x0803f42c
 * If the battery substate is 0x0C, advance it to 9.
 */
void battery_substate_advance(void)
{
    if (G_BAT_STATE[3] == 0x0C) {
        G_BAT_STATE[3] = 9;
    }
}


/*
 * batteryware_update_arm @ 0x0803f450
 * Arm the batteryware-update FSM: set update record uint8_t +6 to 4.
 */
void batteryware_update_arm(void)
{
    G_BATWARE_UPD[6] = 4;
}


/*
 * batteryware_update_set_pending @ 0x0803f420
 * Mark a batteryware update pending: set battery substate (+3) to 0x0F.
 */
void batteryware_update_set_pending(void)
{
    G_BAT_STATE[3] = 0x0F;
}


/*
 * batteryware_update_status_get @ 0x0803f444
 * Read the batteryware-update status uint8_t (update record +7).
 */
uint8_t batteryware_update_status_get(void)
{
    return G_BATWARE_UPD[7];
}
/*
 * bat_modbus_txn_pump @ 0x08039E30
 *
 * Per-call 5-state transaction driver for the battery/BMS Modbus RTU master.
 * Runs one step of the current transaction each call and returns the master
 * step's status (0 = busy/idle, nonzero values propagated from
 * bat_modbus_master_step / state machine).
 *
 * Shared memory model:
 *   DAT_08039F10 = 0x20006E90 -> g_bat_modbus_ctx
 *       [0xE74] = bus/queue handle (pointer)
 *       [0xE78] = transaction-SM state uint8_t
 *       [0xE7C] = request/response frame (== g_bat_frame)
 *       [0xE7D] = frame func uint8_t
 *   DAT_08039F14 = 0x20007D0C -> g_bat_frame (== &g_bat_modbus_ctx[0xE7C])
 *   DAT_08039F18 = 0x200000C2 -> scheduler-slot uint8_t area (+3 -> 0x200000C5)
 *   DAT_08039F1C = 0x40020400 -> GPIOB_BASE
 *   DAT_08039F20 = 0x200000C5 -> scheduler-slot uint8_t (BMS-reset slot id)
 *   DAT_08039F24 = 0x20009D98 -> g_log_func
 *   DAT_08039F28 = 0x0805296C -> "BMS reset end\r\n"
 *
 * bus_queue_pop: generic ring/queue dequeue helper.  Called as
 *   bus_queue_pop(*(void**)(g_bat_modbus_ctx + 0xE74), g_bat_frame)
 * to pull the next queued request frame into g_bat_frame; returns 0 on success.
 */

#define g_bat_frame (g_bat_modbus_ctx + 0xE7C)

uint32_t bat_modbus_txn_pump(void)
{
    uint8_t  slot;
    int32_t  rc;
    uint32_t status;

    status = (uint32_t)g_bat_modbus_ctx[0xE78];   /* transaction-SM state */
    switch (status) {

    case 0:
        /* Dequeue the next request frame from the bus handle into g_bat_frame. */
        rc = bus_queue_pop(*(void **)(g_bat_modbus_ctx + 0xE74),
                          (void *)g_bat_frame);
        if (rc == 0) {
            g_bat_modbus_ctx[0xE78] = 1;
            status = 0;
        }
        break;

    case 1:
        /* Drive the Modbus master uint8_t/frame state machine. */
        status = bat_modbus_master_step((uint8_t *)g_bat_frame);
        if (status == 1) {
            g_bat_modbus_ctx[0xE78] = 2;
        }
        if (((status - 2) & 0xff) < 2) {   /* status == 2 or status == 3 */
            g_bat_modbus_ctx[0xE78] = 3;
        }
        break;

    case 2:
        /* Successful response: if it was a read-holding-registers (func 3),
         * unpack the BMS telemetry into the app/session context. */
        if (*(int8_t *)(g_bat_modbus_ctx + 0xE7D) == 3) {
            bms_telemetry_unpack((uint8_t *)g_bat_frame);
        }
        status = 0;
        g_bat_modbus_ctx[0xE78] = 0;
        break;

    case 3:
        /* Error/abort: just rewind to idle. */
        status = 0;
        g_bat_modbus_ctx[0xE78] = 0;
        break;

    case 4:
        /* BMS hard-reset wait state: assert reset line, run a 4 s timer,
         * then deassert and log. */
        if (*(int8_t *)(0x200000C2u + 3) == -6) {   /* slot id == 0xFA (unallocated) */
            slot = scheduler_alloc();
            *(uint8_t *)(0x200000C2u + 3) = slot;
            scheduler_start(slot, 4000, (sched_cb_t)0);
            HAL_GPIO_WritePin((void *)0x40020400u, 0x20, 1);   /* GPIOB pin 5 high */
        }
        rc = scheduler_slot_is_idle(*(uint8_t *)(0x200000C2u + 3));
        if (rc == 0) {
            status = 0;            /* timer still running */
        }
        else {
            scheduler_release((uint8_t *)0x200000C5u);
            HAL_GPIO_WritePin((void *)0x40020400u, 0x20, 0);   /* GPIOB pin 5 low */
            log_print_timestamp_prefix();
            g_log_func("BMS reset end\r\n");
            status = 0;
            g_bat_modbus_ctx[0xE78] = 0;
        }
        break;

    default:
        status = 0;
        break;
    }

    return status;
}
/*
 * bat_modbus_master_step @ 0x0803996c
 *
 * Modbus-RTU master uint8_t state machine for the BMS slave (0xAA) on the
 * inter-module bus. Builds + CRC-frames a request (func 3/6/0x10) and pumps the
 * response uint8_t-by-uint8_t, verifying the trailing CRC-16.
 *
 * param frame == g_bat_frame (== &g_bat_modbus_ctx[0xE7C]):
 *   [0]      slave addr (0xAA)
 *   [1]      function code
 *   [2..3]   register (big-endian u16)
 *   [4..]    request payload bytes
 *   [0x84]   write uint8_t-count
 *   [0x85+]  response data bytes
 *   [0x105]  expected response length
 *   [0x106]  Modbus exception/error flag
 *
 * Master-SM state lives at g_bat_modbus_ctx[0] (state uint8_t) with a u16 running
 * uint8_t counter at g_bat_modbus_ctx[2].
 *
 * Returns: 1 while busy/idle-continuing, 0 when a frame completed OK,
 *          2 after a final CRC verify, 3 on a Modbus exception response.
 */
unsigned int bat_modbus_master_step(uint8_t *frame)
{
    int16_t  prev_cnt;
    int      got;
    uint16_t flush_cnt;
    uint8_t  func;
    uint16_t crc;
    unsigned int state;
    unsigned int busy;
    uint8_t  rx;

    state = (unsigned int)g_bat_modbus_ctx[0];

    if (state == 8) {
        bus_crc16_verify();
        state = 2;
    } else {
        switch (state) {
        case 0:
            bus_crc16_reset();
            frame[0x106] = 0;

            /* Lazily allocate the inter-frame flush timer slot (0xFA sentinel). */
            if (*(int8_t *)0x200000C4u == (int8_t)0xFA) {
                uint8_t slot = scheduler_alloc();
                *(uint8_t *)0x200000C4u = slot;
                scheduler_set_timer_name(slot, 0, "bmodbus_tmr");
            }

            /* Drain (flush) any stale RX bytes before transmitting. */
            flush_cnt = 0;
            while (((got = bus_rx_byte_locked(&rx)) != 0) &&
                   ((flush_cnt = flush_cnt + 1) < 0x200)) {
                watchdog_kick();
                ((io_fn_t *)0x20009D98u)[1](rx);
            }
            if (10 < flush_cnt) {
                log_print_timestamp_prefix();
                g_log_func("flush BMS rx %d\r\n", flush_cnt);
            }

            scheduler_start(*(uint8_t *)0x200000C4u, 0x5dc, (sched_cb_t)0x0803993dU);

            func = frame[1];
            if (func == 6) {
                /* Write single register: addr,func,reg_hi,reg_lo,val_hi,val_lo */
                bus_tx_enqueue_byte(frame[0]);
                bus_crc16_update(frame[0]);
                bus_tx_enqueue_byte(frame[1]);
                bus_crc16_update(frame[1]);
                bus_tx_enqueue_byte(*(uint16_t *)(frame + 2) >> 8);
                bus_crc16_update(*(uint16_t *)(frame + 2) >> 8);
                bus_tx_enqueue_byte(frame[2]);
                bus_crc16_update(frame[2]);
                bus_tx_enqueue_byte(frame[5]);
                bus_crc16_update(frame[5]);
                bus_tx_enqueue_byte(frame[4]);
                bus_crc16_update(frame[4]);
                crc = bus_crc16_get();
                bus_tx_enqueue_byte(crc & 0xff);
                bus_tx_enqueue_byte((crc & 0xffff) >> 8);
                busy = 1;
                g_bat_modbus_ctx[0] = 1;
            } else if (func == 0x10) {
                /* Write multiple registers. */
                bus_tx_enqueue_byte(frame[0]);
                bus_crc16_update(frame[0]);
                bus_tx_enqueue_byte(frame[1]);
                bus_crc16_update(frame[1]);
                bus_tx_enqueue_byte(*(uint16_t *)(frame + 2) >> 8);
                bus_crc16_update(*(uint16_t *)(frame + 2) >> 8);
                bus_tx_enqueue_byte(frame[2]);
                bus_crc16_update(frame[2]);
                bus_tx_enqueue_byte(0);
                bus_crc16_update(0);
                bus_tx_enqueue_byte(frame[0x84] >> 1);    /* register count = bytes/2 */
                bus_crc16_update(frame[0x84] >> 1);
                bus_tx_enqueue_byte(frame[0x84]);          /* uint8_t count */
                bus_crc16_update(frame[0x84]);
                bus_tx_enqueue_n(frame + 4, frame[0x84]);
                *(uint16_t *)(g_bat_modbus_ctx + 2) = 0;
                while ((unsigned int)*(uint16_t *)(g_bat_modbus_ctx + 2) <
                       (unsigned int)frame[0x84]) {
                    bus_crc16_update(frame[*(uint16_t *)(g_bat_modbus_ctx + 2) + 4]);
                    *(int16_t *)(g_bat_modbus_ctx + 2) =
                        *(int16_t *)(g_bat_modbus_ctx + 2) + 1;
                }
                crc = bus_crc16_get();
                bus_tx_enqueue_byte(crc & 0xff);
                bus_tx_enqueue_byte((crc & 0xffff) >> 8);
                busy = 1;
                g_bat_modbus_ctx[0] = 1;
            } else if (func == 3) {
                /* Read holding registers: addr,func,reg_hi,reg_lo,cnt_hi,cnt_lo */
                bus_tx_enqueue_byte(frame[0]);
                bus_crc16_update(frame[0]);
                bus_tx_enqueue_byte(frame[1]);
                bus_crc16_update(frame[1]);
                bus_tx_enqueue_byte(*(uint16_t *)(frame + 2) >> 8);
                bus_crc16_update(*(uint16_t *)(frame + 2) >> 8);
                bus_tx_enqueue_byte(frame[2]);
                bus_crc16_update(frame[2]);
                bus_tx_enqueue_byte(0);
                bus_crc16_update(0);
                bus_tx_enqueue_byte(frame[0x84]);
                bus_crc16_update(frame[0x84]);
                crc = bus_crc16_get();
                bus_tx_enqueue_byte(crc & 0xff);
                bus_tx_enqueue_byte((crc & 0xffff) >> 8);
                busy = 1;
                g_bat_modbus_ctx[0] = 1;
            } else {
                busy = 1;
            }
            break;

        case 1:
            /* Echo: first response uint8_t must match our slave addr. */
            busy = bus_rx_byte_locked(&rx);
            if (busy == 0) {
                busy = 1;
            } else if (frame[0] == rx) {
                bus_crc16_reset();
                bus_crc16_update(rx);
                g_bat_modbus_ctx[0] = 2;
            } else {
                ((io_fn_t *)0x20009D98u)[1]();   /* discard mismatched uint8_t */
            }
            break;

        case 2:
            /* Function-code uint8_t (high bit set => Modbus exception). */
            busy = bus_rx_byte_locked(&rx);
            if (busy == 0) {
                busy = 1;
            } else {
                if ((int8_t)rx < 0) {
                    frame[0x106] = 1;
                    g_log_func("BMS error rx \r\n");
                    g_bat_modbus_ctx[0] = 8;
                    return state;
                }
                if (frame[1] == (rx & 0x7f)) {
                    bus_crc16_update(rx);   /* fold the function-code byte into the CRC */
                    func = frame[1];
                    if (func == 3) {
                        g_bat_modbus_ctx[0] = 5;          /* read: uint8_t-count next */
                    }
                    if (func == 6) {
                        frame[0x105] = 4;                  /* echo of 4 bytes */
                        *(uint16_t *)(g_bat_modbus_ctx + 2) = 0;
                        g_bat_modbus_ctx[0] = 3;
                    }
                    if (func == 0x10) {
                        frame[0x105] = 4;                  /* echo of 4 bytes */
                        *(uint16_t *)(g_bat_modbus_ctx + 2) = 0;
                        g_bat_modbus_ctx[0] = 4;
                    }
                }
            }
            break;

        case 3:
        case 4:
        case 6:
            /* Collect fixed-length response payload into frame[0x85+]. */
            busy = bus_rx_byte_locked(&rx);
            if (busy == 0) {
                busy = 1;
            } else {
                uint8_t  exp = frame[0x105];
                unsigned int cnt = (unsigned int)*(uint16_t *)(g_bat_modbus_ctx + 2);
                bus_crc16_update(rx);
                if ((cnt < exp) && (cnt < 0x80)) {
                    *(uint16_t *)(g_bat_modbus_ctx + 2) =
                        *(uint16_t *)(g_bat_modbus_ctx + 2) + 1;
                    frame[cnt + 0x85] = rx;
                }
                if ((unsigned int)exp ==
                    (unsigned int)*(uint16_t *)(g_bat_modbus_ctx + 2)) {
                    g_bat_modbus_ctx[0] = 7;
                }
            }
            break;

        case 5:
            /* Read holding registers: uint8_t-count uint8_t. */
            busy = bus_rx_byte_locked(&rx);
            if (busy == 0) {
                busy = 1;
            } else {
                bus_crc16_update(rx);
                frame[0x105] = rx;
                *(uint16_t *)(g_bat_modbus_ctx + 2) = 0;
                if (rx == 0) {
                    g_bat_modbus_ctx[0] = 7;
                } else {
                    g_bat_modbus_ctx[0] = 6;
                }
            }
            break;

        case 7:
            /* Trailing CRC bytes; verify when full frame received. */
            busy = bus_rx_byte_locked(&rx);
            if (busy == 0) {
                busy = 1;
            } else {
                bus_crc16_update(rx);
                prev_cnt = *(int16_t *)(g_bat_modbus_ctx + 2);
                *(int16_t *)(g_bat_modbus_ctx + 2) = prev_cnt + 1;
                if (((uint16_t)(prev_cnt + 1) == (uint16_t)(frame[0x105] + 2)) &&
                    (bus_crc16_get() == 0)) {
                    bus_crc16_verify();
                    busy = 0;
                    if (frame[0x106] != 0) {
                        return 3;
                    }
                }
            }
            break;

        default:
            busy = 1;
        }
        state = (busy ^ 1) & 0xff;
    }
    return state;
}
/*
 * bms_telemetry_unpack @ 0x0803dc0c
 *
 * Parse the func-3 (read-holding-registers) response data bytes from the BMS
 * Modbus frame into the G_APP_CTX register cache, big-endian u16 per register.
 *
 * param: pointer to the Modbus frame (== g_bat_frame).
 *   frame[2..3]  = base register number (read here as a native-order u16; the
 *                  field is the host-side starting register index)
 *   frame[0x105] = response uint8_t-count (data length); registers = len/2
 *   frame[0x85 + i] = response payload bytes, big-endian per register:
 *                     value = (frame[0x85+i] << 8) | frame[0x86+i]
 *
 * Each register N is stored into the app context register cache at
 *   *(uint16_t*)(G_APP_CTX + (N + 0x1f8)*2 + 2)   (== G_APP_CTX + 0x3F2 + N*2)
 *
 * Side-effects on specific registers:
 *   reg 5  -> mirror low uint8_t of cache[0x3fc] into G_APP_CTX[0x315]
 *   reg 11 -> if serial-number field (G_APP_CTX+0x408) is nonzero and differs
 *             from the cached "known" version (G_APP_CTX+0x342), log
 *             "Update BMS version in EEPROM\r\n" and latch the new value.
 *   If the verbose-dump flag (G_APP_CTX+0x34f) is set, dump every parsed
 *   register as "MBB 0x%04X 0x%02X " followed by "\r\n".
 */
void bms_telemetry_unpack(uint8_t *frame)
{
    uint8_t *ctx = (uint8_t *)G_APP_CTX;   /* 0x200083A8 */
    int i;
    uint32_t base_reg;

    for (i = 0; i < (int)(uint32_t)frame[0x105]; i += 2) {
        int idx = i / 2;
        uint32_t reg;

        base_reg = (uint32_t)*(uint16_t *)(frame + 2);
        reg = base_reg + (uint32_t)idx;

        /* big-endian u16: high uint8_t at [0x85+i], low uint8_t at [0x86+i] */
        *(uint16_t *)(ctx + (reg + 0x1f8) * 2 + 2) =
            (uint16_t)(((uint16_t)frame[0x85 + i] << 8) | (uint16_t)frame[0x86 + i]);

        if (reg == 5) {
            *(int8_t *)(ctx + 0x315) = (int8_t)*(uint16_t *)(ctx + 0x3fc);
        }

        if ((reg == 0xb) &&
            ((int16_t)*(int16_t *)(ctx + 0x408) != 0) &&
            ((uint16_t)*(uint16_t *)(ctx + 0x342) != (int16_t)*(int16_t *)(ctx + 0x408))) {
            log_print_timestamp_prefix();
            g_log_func("Update BMS version in EEPROM\r\n");
            *(uint16_t *)(ctx + 0x342) = *(uint16_t *)(ctx + 0x408);
        }
    }

    if (*(int8_t *)(ctx + 0x34f) != '\0') {
        uint32_t r;

        log_print_timestamp_prefix();
        for (r = (uint32_t)*(uint16_t *)(frame + 2);
             (int)r < (int)((uint32_t)*(uint16_t *)(frame + 2) +
                            (uint32_t)(frame[0x105] >> 1));
             r++) {
            g_log_func("MBB 0x%04X 0x%02X ",
                       r,
                       (int)*(int16_t *)(ctx + (r + 0x1f8) * 2 + 2));
        }
        g_log_func("\r\n");
    }

    return;
}
/* ====================================================================
 * VanMoof S3 mainware (STM32F413) - battery / BMS Modbus driver
 * Group R5: service step + charge-state display step
 *
 * Shared RAM state for these two functions lives at 0x20008A00.
 * It is a small driver struct, distinct from g_bat_modbus_ctx
 * (0x20006E90):
 *   [0x00] void*   cached app context pointer (= G_APP_CTX param_1)
 *   [0x04] int16_t last charge-% reported to the SSP/telemetry layer
 *   [0x3F] uint8_t consecutive-error retry counter
 * Both functions take param_1 = G_APP_CTX (0x200083A8). The app-ctx
 * register area offsets used here:
 *   +0x3B2 : current charge percentage (uint16)
 *   +0x3F8 : pack voltage / "level" raw (int16)
 *   +0x3FC : charging state code (int16, -1 == unknown/absent)
 *   +0x3FE : (int16) charge-timer / aux value
 *   +0x442 : (uint16) aux flag word
 * ==================================================================== */


/* RAM driver state at 0x20008A00 (see header). */
#define g_modbus_bat_state ((uint8_t *)0x20008A00u)

/* divide-by-10 reciprocal constant (0xCCCCCCCD) for charge%% -> tens/ones */
#define BAT_DIV10_RECIP 0xCCCCCCCDuLL

/* rodata: power-state name table, base 0x0804F47C, indexed [state*4 + 0xB4].
 * Entries point to: "PWR_UNKNOWN","PWR_BAT_EMPTY","PWR_SAVING",
 * "PWR_LIMITED_BAT_LOW","PWR_BAT_LOW","PWR_LIMITED","PWR_NORMAL". */

/* ---------------------------------------------------------------------
 * modbus_bat_service_step @ 0x0803F338
 * Per-super-loop service for the battery (BMS) Modbus master link.
 * Pumps the transaction state machine once (bat_modbus_txn_pump) and
 * reacts to its status:
 *   3 = rejected -> log "Modbus Bat reject"
 *   2 = error    -> log " ERR Modbus Bat", bump retry counter; on the
 *                   6th+ consecutive error (old value > 4) re-init the
 *                   link (bat_modbus_reinit; if it returns 0 log
 *                   " ERR MBB flush"), raise the 0x80000 state flag, and
 *                   log " ERR MBB reset".
 *   1 = success  -> clear retry counter and clear the 0x80000 state flag
 *                   (arms the next transaction).
 * Then runs the telemetry / state / charge-display sub-steps.
 * 0x80000 is the battery-Modbus state-flag bit (distinct from BLE/SSP).
 * ------------------------------------------------------------------- */
void modbus_bat_service_step(uint32_t param_1, uint32_t param_2,
                             uint32_t param_3, uint32_t param_4)
{
    uint8_t prev_errcount;
    int status;
    int reinit_ok;

    (void)param_2;
    (void)param_3;

    /* cache the app context pointer in the driver state cell */
    *(uint32_t *)g_modbus_bat_state = param_1;

    status = bat_modbus_txn_pump();

    if (status == 3) {
        /* rejected */
        g_log_func("Modbus Bat reject\r\n");
    }

    if (status == 2) {
        /* transaction error */
        log_print_timestamp_prefix();
        g_log_func(" ERR Modbus Bat\r\n");

        prev_errcount = g_modbus_bat_state[0x3f];
        g_modbus_bat_state[0x3f] = (uint8_t)(prev_errcount + 1);

        if (prev_errcount > 4) {
            /* 6th+ consecutive error: full reset of the link */
            reinit_ok = bat_modbus_reinit();
            if (reinit_ok == 0) {
                g_log_func(" ERR MBB flush\r\n");
            }
            state_flags_set(0x80000, 0);
            log_print_timestamp_prefix();
            g_log_func(" ERR MBB reset\r\n");
        }
    }
    else if (status == 1) {
        /* success: clear retry counter, clear state flag (arm next txn) */
        g_modbus_bat_state[0x3f] = 0;
        state_flags_clear(0x80000, 0, 0,
                          *(uint32_t *)g_modbus_bat_state, param_4);
    }

    battery_telemetry_step(param_1);
    battery_state_process(param_1, status);
    battery_charge_display_step(param_1);
}

/* ---------------------------------------------------------------------
 * battery_charge_display_step @ 0x0803E4BC
 * Tracks the BMS-reported charge percentage and drives both the
 * telemetry/SSP message (when the percentage changes) and the
 * charge-display / power-path state machine.
 *
 * param_1 = G_APP_CTX (0x200083A8).
 *
 * On a change of charge% (ctx+0x3B2 vs cached state[+4]):
 *   - log "Set power state to %s (Current limit: %d.%d A, SOC: %d %%)"
 *     where %s = power-state name, %d.%d = current limit table entry,
 *     SOC = tens.ones split of the charge% (via the 0xCCCCCCCD div-10).
 *   - enqueue a 2-uint8_t telemetry message (cmd 0x19) carrying ctx+0x3B2;
 *     if the enqueue depth exceeds 0x10, log "ERROR SSPM place".
 *
 * Then, based on the charging-state code (ctx+0x3FC):
 *   -1  : no battery -> force charge% to 200 (sentinel)
 *   else: dispatch on the clamped power state (0..6):
 *         0,3,4,5,6 : run the normal charge-curve helper (battery_charge_lookup);
 *                     if it returns >=0 latch the resulting display code
 *                     (battery_set_charge_mode); if that code == 6, kick the
 *                     charge-timer scheduler (battery_charge_complete_watchdog).
 *         1 : reset charge% to 0; if helper result > 3, set display 2.
 *         2 : reset charge% to 0; if helper result > 9, set display 4;
 *             if helper result < 4, set display 1.
 *         default : set display 0.
 * ------------------------------------------------------------------- */
void battery_charge_display_step(int param_1)
{
    int16_t  level_raw;       /* ctx+0x3F8 */
    uint16_t aux_flags;       /* ctx+0x442 */
    int16_t  charge_state;    /* ctx+0x3FC */
    uint32_t state_for_disp;
    int      tens;
    uint16_t *ctx_charge_field; /* &ctx[0x3B2] */
    int      charge_state_i;
    char     disp_code[5];    /* local_21: latched display code uint8_t */

    ctx_charge_field = (uint16_t *)(param_1 + 0x3b2);
    aux_flags        = *(uint16_t *)(param_1 + 0x442);
    level_raw        = *(int16_t  *)(param_1 + 0x3f8);
    charge_state     = *(int16_t  *)(param_1 + 0x3fc);
    charge_state_i   = (int)charge_state;

    /* report a change in charge percentage */
    if (*(int16_t *)(param_1 + 0x3b2) != *(int16_t *)(g_modbus_bat_state + 4)) {
        uint32_t depth;
        int      pstate;

        *(int16_t *)(g_modbus_bat_state + 4) = *(int16_t *)(param_1 + 0x3b2);

        log_print_timestamp_prefix();
        pstate = power_state_get_clamped();
        tens = (int)((BAT_DIV10_RECIP *
                      (uint64_t)(uint32_t)*(uint16_t *)(param_1 + 0x3b2)) >> 0x23);
        g_log_func("Set power state to %s (Current limit: %d.%d A, SOC: %d %%)\r\n",
               g_power_state_name_table[pstate],
               (uint32_t)tens,
               ((uint32_t)*(uint16_t *)(param_1 + 0x3b2) + (uint32_t)(tens * -10)) & 0xffff,
               charge_state_i);

        depth = maybe_enqueue_tx_message(0x19, 2, ctx_charge_field, 0);
        if (0x10 < depth) {
            g_log_func("ERROR SSPM place\r\n");
        }
    }

    if (charge_state_i == -1) {
        /* no battery present -> sentinel charge value */
        *(uint16_t *)(param_1 + 0x3b2) = 200;
    }
    else {
        state_for_disp = (uint32_t)power_state_get_clamped();
        disp_code[0] = (char)state_for_disp;

        switch (state_for_disp) {
        case 0:
        case 3:
        case 4:
        case 5:
        case 6:
            charge_state_i = battery_charge_lookup((int)charge_state, aux_flags,
                                          (int)(int16_t)(level_raw - 0xaab),
                                          ctx_charge_field, disp_code);
            if (-1 < charge_state_i) {
                battery_set_charge_mode(disp_code[0]);
            }
            if (disp_code[0] == '\x06') {
                battery_charge_complete_watchdog((int)*(int16_t *)(param_1 + 0x3fe), ctx_charge_field);
            }
            break;
        case 1:
            *(uint16_t *)(param_1 + 0x3b2) = 0;
            if (3 < charge_state_i) {
                battery_set_charge_mode(2);
            }
            break;
        case 2:
            *(uint16_t *)(param_1 + 0x3b2) = 0;
            if (9 < charge_state_i) {
                battery_set_charge_mode(4);
            }
            if (charge_state_i < 4) {
                battery_set_charge_mode(1);
            }
            break;
        default:
            battery_set_charge_mode(0);
            break;
        }
    }
}

/*
 * battery_telemetry_step @ 0x0803EB3C — the periodic BMS-over-Modbus engine.
 *
 * Switch on the BMS substate G_BAT_STATE[3] (0x200000E7): probe/detect the pack
 * (cases 0-3), reset/sleep handling (case 4), bring-up + discharge enable (case
 * 6/7), charger/discharge edge reporting (case 8), the BMS firmware-reset
 * sequence (cases 9-0xB), the steady-state telemetry poll (case 0xC), and the
 * power-off path (case 0xF). param_1 is the app/session context (g_app_ctx); the
 * BMS state object, its timer slots ([0]/[5]/[7]) and counters live in
 * G_BAT_STATE, the batteryware-update record in G_BATWARE_UPD (+0x3E = charger
 * shadow). GPIOC 0x40020800 (PC10 present / PC4 charger), GPIOB 0x40020400 (BMS
 * reset/power), GPIOD 0x40020C00 (PD1 sense).
 */
void battery_telemetry_step(int param_1)
{
    uint8_t *st = G_BAT_STATE;
    uint8_t  local_11[5];
    int      iVar8;
    uint32_t uVar7;
    uint8_t  uVar5, uVar3, bVar4;
    char     cVar9;

    switch (st[3]) {
    case 0:
        if (scheduler_slot_is_idle(st[5]) != 0) {
            scheduler_start(st[5], 500, 0);
            bat_modbus_reinit();
            log_print_timestamp_prefix();
            g_log_func("BMS ask ID\r\n");
            bms_modbus_read(0, 1);
            uVar5 = maybe_get_bike_state();
            if (uVar5 == 0x19) {
                bms_modbus_read(0xb, 1);
            }
            st[3] = 3;
        }
        break;

    case 1:
        iVar8 = HAL_GPIO_ReadPin((void *)0x40020800u, 0x400);   /* PC10 present */
        if (iVar8 == 0) {
            if ((int8_t)st[5] == -6) {
                log_print_timestamp_prefix();
                g_log_func("No BMS detected\r\n");
                st[3] = 2;
                if ((int8_t)st[5] == -6) {
                    bVar4 = scheduler_alloc();
                    st[5] = bVar4;
                    scheduler_set_timer_name(bVar4, 600, "pwron_tmr");
                    scheduler_start(st[5], 600, 0);
                }
            }
        } else {
            if ((int8_t)st[5] == -6) {
                HAL_GPIO_WritePin((void *)0x40020400u, 0x20, 1);   /* BMS reset pulse */
                systick_delay(10);
                HAL_GPIO_WritePin((void *)0x40020400u, 0x20, 0);
                *(uint16_t *)(param_1 + 0x402) = 0;
                *(uint16_t *)(param_1 + 0x3f4) = 0;
                *(uint16_t *)(param_1 + 0x3f2) = 0;
                log_print_timestamp_prefix();
                g_log_func("BMS Pulse\r\n");
                bat_modbus_reinit();
                bVar4 = scheduler_alloc();
                st[5] = bVar4;
                scheduler_set_timer_name(bVar4, 0x5dc, "pwron_tmr");
                scheduler_start(st[5], 0x5dc, 0);
            }
            st[3] = 0;
        }
        break;

    case 2:
        iVar8 = HAL_GPIO_ReadPin((void *)0x40020800u, 0x400);   /* PC10 present */
        if (iVar8 != 0) {
            log_print_timestamp_prefix();
            g_log_func("BMS detected\r\n");
            st[3] = 1;
            scheduler_release(st + 5);
            state_flags_clear(0x40000, 0);
        }
        if (scheduler_slot_is_idle(st[5]) != 0) {
            state_flags_set(0x40000, 0);
        }
        break;

    case 3:
        if (*(int16_t *)(param_1 + 0x3f2) == 0x100) {
            scheduler_start(st[5], 500, 0);
            st[3] = 6;
            state_flags_clear(0x80000, 0);
        }
        if (scheduler_slot_is_idle(st[5]) != 0) {
            cVar9 = (char)(st[6] - 1);
            st[6] = cVar9;
            if (cVar9 == 0) {
                log_print_timestamp_prefix();
                g_log_func("BMS: no ID\r\n");
                scheduler_release(st + 5);
                st[3] = 1;
            } else {
                scheduler_start(st[5], 1000, 0);
                st[3] = 0;
            }
        }
        break;

    case 4:
        if (st[7] == 0xfa) {
            uVar5 = scheduler_alloc();
            st[7] = uVar5;
            scheduler_start(uVar5, 1000, 0);
        }
        iVar8 = HAL_GPIO_ReadPin((void *)0x40020C00u, 2);   /* PD1 sense */
        if (iVar8 == 0) {
            log_print_timestamp_prefix();
            g_log_func("BMS in sleep mode\r\n");
            st[3] = 5;
            scheduler_release(st + 7);
        }
        if (scheduler_slot_is_idle(st[7]) != 0) {
            scheduler_release(st + 7);
            if (st[6] == 0) {
                st[3] = 5;
                state_flags_set(0x80000, 0);
            } else {
                st[6] = st[6] - 1;
                st[3] = 0x0f;
            }
        }
        break;

    case 6:
        if (scheduler_slot_is_idle(st[5]) != 0) {
            uVar5 = st[5];
            uVar3 = maybe_get_bike_state();
            uVar7 = (uVar3 == 0x15) ? 1000 : 5000;
            scheduler_start(uVar5, uVar7, 0);
            log_print_timestamp_prefix();
            g_log_func("BMS set discharge\r\n");
            *(uint16_t *)(param_1 + 0x402) = 0;
            status_clear_pulse_flag();
            HAL_GPIO_WritePin((void *)0x40020C00u, 0x2000, 0);   /* PD13 */
            bms_modbus_write(8, 1);
            bms_modbus_write(9, 0);
            bms_modbus_read(8, 1);
            iVar8 = motor_fw_update_fsm_step();
            if (iVar8 == 3) {
                HAL_GPIO_WritePin((void *)0x40020400u, 0x400, 1);   /* PB10 */
                log_print_timestamp_prefix();
                g_log_func("Motor reset\r\n");
                HAL_GPIO_WritePin((void *)0x40020400u, 0x200, 1);   /* PB9 pulse */
                systick_delay(10);
                HAL_GPIO_WritePin((void *)0x40020400u, 0x200, 0);
            }
            st[3] = 7;
        }
        break;

    case 7:
        iVar8 = scheduler_slot_is_idle(st[5]);
        if ((iVar8 != 0) ||
            ((*(int16_t *)(param_1 + 0x402) != 0) &&
             (uVar5 = maybe_get_bike_state(), uVar5 != 0x15))) {
            battery_request_telemetry();
            scheduler_start(st[5], 2000, 0);
            st[3] = 8;
        }
        break;

    case 8:
        iVar8 = HAL_GPIO_ReadPin((void *)0x40020800u, 0x10);   /* PC4 charger */
        if (((iVar8 != 0) && (*(int16_t *)(param_1 + 0x402) == 1)) ||
            ((iVar8 = HAL_GPIO_ReadPin((void *)0x40020800u, 0x10), iVar8 == 0) &&
             (*(int16_t *)(param_1 + 0x3f4) == 1))) {
            uVar7 = maybe_enqueue_tx_message(10, 0, 0, 1);
            if (0x10 < uVar7) {
                g_log_func("  ERROR SSPM1 place\r\n");
            }
            state_flags_clear(0x20000, 0);
            scheduler_start(st[5], 1000, 0);
            st[3] = 0x0c;
        }
        if (scheduler_slot_is_idle(st[5]) != 0) {
            log_print_timestamp_prefix();
            iVar8 = HAL_GPIO_ReadPin((void *)0x40020800u, 0x10);
            if (iVar8 == 0) {
                g_log_func("BMS: No CHG\r\n");
            } else {
                g_log_func("BMS: No DSG\r\n");
            }
            scheduler_start(st[5], 200, 0);
            state_flags_set(0x20000, 0);
            *(uint16_t *)(param_1 + 0x3f6) = 0;
            bms_modbus_read(2, 1);
            st[3] = 0x0e;
        }
        break;

    case 9:
        if (st[7] == 0xfa) {
            bVar4 = scheduler_alloc();
            st[7] = bVar4;
            scheduler_set_timer_name(bVar4, 0x5dc, "bms_reset_tmr");
            scheduler_start(st[7], 0x5dc, 0);
            log_print_timestamp_prefix();
            g_log_func("ADC Vbat %d\r\n", *(uint16_t *)(param_1 + 0x3b0));
            memset((void *)(param_1 + 0x3f2), 0, 300);
            *(uint16_t *)(param_1 + 0x3fc) = 0xffff;
            battery_request_telemetry();
            if (st[0] == 0xfa) {
                uVar5 = scheduler_alloc();
                st[0] = uVar5;
                scheduler_start(uVar5, 1000, (sched_cb_t)0x0803e381u);
            }
            uVar7 = maybe_enqueue_tx_message(0xc, 0, 0, 1);
            if (0x10 < uVar7) {
                g_log_func("  ERROR SSP place\r\n");
            }
        }
        if (scheduler_slot_is_idle(st[7]) != 0) {
            scheduler_release(st + 7);
            if (*(uint16_t *)(param_1 + 0x3b0) < 0x61a9) {
                st[3] = 10;
            } else {
                log_print_timestamp_prefix();
                g_log_func("BMS oke %d\r\n", *(uint16_t *)(param_1 + 0x3b0));
                st[3] = 0x0c;
            }
        }
        break;

    case 10:
        if (st[7] == 0xfa) {
            log_print_timestamp_prefix();
            g_log_func("BMS Resetting %d\r\n", *(uint16_t *)(param_1 + 0x3b0));
            sched_timer_arm_or_alloc(15000);
            shift_aux_state_reset();
            st[8] = *(uint8_t *)(param_1 + 0x10c);
            *(uint8_t *)(param_1 + 0x10c) = 2;
            HAL_GPIO_WritePin((void *)0x40020400u, 0x20, 1);   /* BMS reset on */
            bVar4 = scheduler_alloc();
            st[7] = bVar4;
            scheduler_set_timer_name(bVar4, 0xdac, "bms_reset_tmr");
            scheduler_start(st[7], 0xdac, 0);
        }
        if (scheduler_slot_is_idle(st[7]) != 0) {
            scheduler_start(st[7], 2000, 0);
            HAL_GPIO_WritePin((void *)0x40020400u, 0x20, 0);   /* BMS reset off */
            st[3] = 0x0b;
        }
        break;

    case 0x0b:
        if (bus_rx_byte_locked(local_11) != 0) {
            ((io_fn_t *)0x20009D98u)[1](local_11[0]);   /* drain RX through the IO sink */
        }
        if (scheduler_slot_is_idle(st[7]) != 0) {
            scheduler_release(st + 7);
            scheduler_release(st + 5);
            log_print_timestamp_prefix();
            g_log_func("BMS start again\r\n");
            st[6] = 10;
            st[3] = 1;
        }
        break;

    case 0x0c:
        if (scheduler_slot_is_idle(st[5]) != 0) {
            if (*(int16_t *)(param_1 + 0x3fc) == -1) {
                log_print_timestamp_prefix();
                g_log_func("BMS: No SOC\r\n");
                state_flags_set(0, 0x8000);
                console_cmd_battery(0);
                scheduler_start(st[5], 0x4b0, 0);
                st[3] = 0x0d;
            } else {
                scheduler_release(st + 5);
                state_flags_clear(0, 0x8000);
            }
        }
        if (st[0] == 0xfa) {
            uVar5 = scheduler_alloc();
            st[0] = uVar5;
        }
        if (scheduler_slot_is_idle(st[0]) != 0) {
            uVar7 = power_state_get_clamped();
            if (uVar7 < 6) {
                scheduler_start(st[0], 10000, 0);
            } else {
                scheduler_start(st[0], 60000, 0);
            }
            bms_modbus_read(3, 4);
        }
        if ((100 < *(uint16_t *)(param_1 + 0x3c2)) &&
            (uVar7 = scheduler_slot_get_remaining(st[0]), 1000 < uVar7)) {
            scheduler_start(st[0], 1000, 0);
        }
        if (st[8] != 3) {
            g_log_func("Restore light mode\r\n");
            *(uint8_t *)(param_1 + 0x10c) = st[8];
            st[8] = 3;
        }
        cVar9 = *(char *)(G_BATWARE_UPD + 0x3e);
        iVar8 = HAL_GPIO_ReadPin((void *)0x40020C00u, 2);   /* PD1 sense */
        if ((bool)cVar9 != (iVar8 == 0)) {
            iVar8 = HAL_GPIO_ReadPin((void *)0x40020C00u, 2);
            *(bool *)(G_BATWARE_UPD + 0x3e) = (iVar8 == 0);
            log_print_timestamp_prefix();
            iVar8 = HAL_GPIO_ReadPin((void *)0x40020C00u, 2);
            if (iVar8 == 0) {
                g_log_func(" ERR battery FAULT PIN\r\n");
            } else {
                g_log_func(" CLR battery FAULT PIN\r\n");
            }
            *(uint16_t *)(param_1 + 0x3f6) = 0;
            bms_modbus_read(2, 1);
            bms_modbus_read(0x28, 1);
        }
        battery_fault_warning_report(param_1);
        st[6] = 10;
        break;

    case 0x0d:
        if (scheduler_slot_is_idle(st[5]) != 0) {
            scheduler_start(st[5], 100, 0);
            st[3] = 1;
        }
        break;

    case 0x0e:
        if (scheduler_slot_is_idle(st[5]) != 0) {
            battery_fault_warning_report(param_1);
            scheduler_start(st[5], 100, 0);
            st[3] = 1;
        }
        break;

    case 0x0f:
        log_print_timestamp_prefix();
        g_log_func("BMS off\r\n");
        bms_modbus_write(1, 0);
        iVar8 = HAL_GPIO_ReadPin((void *)0x40020800u, 0x400);   /* PC10 present */
        if (iVar8 == 0) {
            st[3] = 5;
        } else {
            st[3] = 4;
        }
    }
}

/*
 * battery_state_process @ 0x0803e678
 *
 * Battery / BMS firmware-update + presence state machine. Drives the
 * Modbus RTU master toward BMS slave 0xAA: validates the staged BMS pack
 * (PACK header / type uint8_t 0xB1), erases the BMS flash, streams 0x80-uint8_t
 * write blocks (func 0x10, reg 0x82), polls progress, then issues a CRC
 * check (func read 0x81) and finally a reset/internal-update command
 * (func 0x06, reg 0x80).
 *
 * Shared memory:
 *   g_bat_update_ctx = (uint8_t*)0x20008A00  -- the update/session control block
 *     [6]    = inner update sub-state (the switch selector)
 *     [7]    = outer/result state written on completion
 *     [8..]  = staged pack header (validated by pack_validate)
 *     [0xc]  = packed type/version word (type in low uint8_t, version BCD bytes)
 *     [0x14] = staged image size (bytes)
 *     [0x18] = build-date string ptr arg
 *     [0x24] = build-time string ptr arg
 *     [0x30] = current write offset into the staged image
 *     [0x34] = start systick (throughput calc)
 *     [0x38] = per-block write retry counter
 *   g_bat_sched_slot = (uint8_t*)0x20006EE7  -- holds the timer slot at [+4]
 *   g_log_func       = the log/printf-style sink
 *   param_1          = the app/session context (BMS modbus answer code at +0x4f4)
 *   param_2          = event flag (1 == scheduler timer fired)
 */

#define g_bat_update_ctx   ((uint8_t *)0x20008A00u)
#define g_bat_sched_slot   ((uint8_t *)0x20006EE7u)

void battery_state_process(int param_1, int param_2)
{
    short  answer;
    uint32_t type_word;
    uint8_t slot;
    int    rc;
    uint32_t size;
    uint32_t now;

    /* Modbus write frame staged on the stack:
     *   [0]=slave(0xAA) [1]=func [2..3]=reg(BE) [4]=uint8_t-count [5..]=payload */
    uint8_t  frame[0x85];   /* local_120 .. local_9c (+0x84) */

    switch (g_bat_update_ctx[6]) {

    case 4: /* validate staged BMS pack header */
        g_bat_update_ctx[7] = 0;
        rc = pack_validate((uint32_t *)(g_bat_update_ctx + 8), 0x080c0000u);
        if (rc == 0) {
            type_word = *(uint32_t *)(g_bat_update_ctx + 0xc);
            if ((type_word & 0xff) == 0xb1) {
                /* "Batteryware: v%d.%02d.%02d (%s %s)" */
                g_log_func("Batteryware: v%d.%02d.%02d (%s %s)\r\n",
                           type_word >> 0x18,
                           (type_word & 0xffffff) >> 0x10,
                           (type_word & 0xffff) >> 8,
                           (void *)(g_bat_update_ctx + 0x18),
                           (void *)(g_bat_update_ctx + 0x24));
                /* " size %d bytes\r\n" */
                g_log_func(" size %d bytes\r\n",
                           *(uint32_t *)(g_bat_update_ctx + 0x14));
                g_bat_update_ctx[6] = 5;
            } else {
                /* "Not a BMS file\r\n" */
                g_log_func("Not a BMS file\r\n");
                g_bat_update_ctx[6] = 0x10;
            }
        } else {
            /* "BMS CRC error\r\n" */
            g_log_func("BMS CRC error\r\n");
            g_bat_update_ctx[6] = 0x10;
        }
        break;

    case 5: /* command BMS flash erase */
        log_print_timestamp_prefix();
        /* "Send battery erase\r\n" */
        g_log_func("Send battery erase\r\n");
        bms_modbus_write(0x95, 1);
        if ((int8_t)g_bat_sched_slot[4] == -6) {   /* 0xFA == unallocated */
            slot = scheduler_alloc();
            g_bat_sched_slot[4] = slot;
            scheduler_set_timer_name(slot, 4000, "update_tmr");
        }
        scheduler_start(g_bat_sched_slot[4], 4000, 0);
        *(uint32_t *)(g_bat_update_ctx + 0x30) = 0;
        g_bat_update_ctx[6] = 7;
        break;

    case 7: /* wait for erase to complete */
        if (param_2 == 1) {
            now = systick_now();
            *(uint32_t *)(g_bat_update_ctx + 0x34) = now;
            g_bat_update_ctx[6] = 8;
        }
        rc = scheduler_slot_is_idle(g_bat_sched_slot[4]);
        if (rc != 0) {
            /* "  MB Erase timeout\r\n" */
            g_log_func("  MB Erase timeout\r\n");
            g_bat_update_ctx[6] = 0x10;
        }
        break;

    case 8: /* submit one 0x80-uint8_t write block */
    {
        uint32_t off;
        int i;
        frame[0]    = 0xaa;                 /* slave 0xAA */
        frame[1]    = 0x10;                 /* func: write multiple */
        *(uint16_t *)(frame + 2) = 0x82;    /* reg 0x0082 (LE store of recovered value) */
        frame[0x84] = 0x24;                 /* uint8_t count placeholder */
        off = *(uint32_t *)(g_bat_update_ctx + 0x30);
        /* reg/address field big-endian from current offset */
        frame[4] = (uint8_t)(off >> 0x18);
        frame[5] = (uint8_t)(off >> 0x10);
        frame[6] = (uint8_t)(off >> 8);
        frame[7] = (uint8_t)off;
        /* copy 0x20 bytes of payload from staged image at 0x080c0000+off */
        for (i = 0; i < 8; i++)
            *(uint32_t *)(frame + 8 + i * 4) =
                *(uint32_t *)(0x080c0000u + off + i * 4);
        rc = modbus_bat_submit(frame);
        if (rc != 0) {
            /* "  MB Error\r\n" */
            g_log_func("  MB Error\r\n");
            g_bat_update_ctx[6] = 0x10;
        }
        *(uint32_t *)(g_bat_update_ctx + 0x30) += 0x20;
        scheduler_start(g_bat_sched_slot[4], 2000, 0);
        g_bat_update_ctx[0x38] = 3;         /* reset retry budget */
        g_bat_update_ctx[6] = 9;
        break;
    }

    case 9: /* wait for write ack, advance / retry */
        if ((param_2 == 1) && (bus_queue_peek_status() == 0)) {
            state_flag_set(0);
            /* "%d/%d\r" -- progress (written / total) */
            g_log_func("%d/%d\r",
                       *(uint32_t *)(g_bat_update_ctx + 0x30),
                       *(uint32_t *)(g_bat_update_ctx + 0x14));
            state_flag_set(1);
            if (*(uint32_t *)(g_bat_update_ctx + 0x30) <
                *(uint32_t *)(g_bat_update_ctx + 0x14)) {
                g_bat_update_ctx[6] = 8;        /* more blocks */
            } else {
                g_bat_update_ctx[6] = 10;       /* done streaming */
            }
        }
        rc = scheduler_slot_is_idle(g_bat_sched_slot[4]);
        if (rc != 0) {
            if ((int8_t)g_bat_update_ctx[0x38] < 1) {
                /* "  MB Write timeout\r\n" */
                g_log_func("  MB Write timeout\r\n");
                g_bat_update_ctx[6] = 0x10;
            } else {
                g_bat_update_ctx[0x38] = (uint8_t)(g_bat_update_ctx[0x38] - 1);
                scheduler_start(g_bat_sched_slot[4], 1000, 0);
                /* "  MB Write retry\r\n" */
                g_log_func("  MB Write retry\r\n");
                rc = modbus_bat_submit(frame);
                if (rc != 0) {
                    /* "  MB Error\r\n" */
                    g_log_func("  MB Error\r\n");
                    g_bat_update_ctx[6] = 0x10;
                }
            }
        }
        break;

    case 10: /* report throughput, request CRC check */
        log_print_timestamp_prefix();
        size = *(uint32_t *)(g_bat_update_ctx + 0x14);
        now  = systick_now();
        /* "Written %d bytes/sec\r\n" -- size / ((now-start)/1000) via
         * the recovered fixed-point divide-by-1000 (magic 0x10624dd3). */
        g_log_func("Written %d bytes/sec\r\n",
                   (uint32_t)((uint64_t)size /
                       (((uint64_t)0x10624dd3u *
                         (uint64_t)(now - *(uint32_t *)(g_bat_update_ctx + 0x34))) >> 0x26)));
        log_print_timestamp_prefix();
        /* "Send BMS crc check\r\n" */
        g_log_func("Send BMS crc check\r\n");
        bms_modbus_read(0x81, 1);
        *(uint16_t *)(param_1 + 0x4f4) = 0x2a6;     /* arm pending answer */
        scheduler_start(g_bat_sched_slot[4], 2000, 0);
        g_bat_update_ctx[6] = 0xc;
        break;

    case 0xc: /* evaluate BMS CRC-check answer */
        answer = *(short *)(param_1 + 0x4f4);
        if (answer != 0x2a6) {              /* still pending? */
            if (answer == 1) {
                log_print_timestamp_prefix();
                /* "crc check ok\r\n" */
                g_log_func("crc check ok\r\n");
                g_bat_update_ctx[6] = 0x10;
            } else if (answer == 2) {
                log_print_timestamp_prefix();
                /* "checksum error\r\n" */
                g_log_func("checksum error\r\n");
                g_bat_update_ctx[6] = 0x10;
            } else if (answer == 0) {
                log_print_timestamp_prefix();
                /* "No app header\r\n" */
                g_log_func("No app header\r\n");
                g_bat_update_ctx[6] = 0x11;
            } else {
                log_print_timestamp_prefix();
                /* "Unknown answer (%d)\r\n" */
                g_log_func("Unknown answer (%d)\r\n",
                           (int)*(short *)(param_1 + 0x4f4));
                g_bat_update_ctx[6] = 0x10;
            }
        }
        rc = scheduler_slot_is_idle(g_bat_sched_slot[4]);
        if (rc != 0) {
            /* "  Ask crc timeout\r\n" */
            g_log_func("  Ask crc timeout\r\n");
            g_bat_update_ctx[6] = 0x10;
        }
        break;

    case 0x10: /* terminal: release timer, signal result 2 */
        scheduler_release(g_bat_sched_slot + 4);
        g_bat_update_ctx[7] = 2;
        break;

    case 0x11: /* "No app header" path -> command BMS reset/internal update */
        log_print_timestamp_prefix();
        /* "cmd reset BMS\r\n" */
        g_log_func("cmd reset BMS\r\n");
        frame[0]    = 0xaa;     /* slave 0xAA */
        frame[1]    = 6;        /* func: write single register */
        *(uint16_t *)(frame + 2) = 0x80;   /* reg 0x0080 */
        frame[0x84] = 1;
        frame[4]    = 1;        /* value hi = 1 */
        frame[5]    = 0;        /* value lo = 0 */
        rc = modbus_bat_submit(frame);
        if (rc != 0) {
            /* "  MB Error\r\n" */
            g_log_func("  MB Error\r\n");
        }
        /* "Wait for BMS internal update!\r\n" */
        g_log_func("Wait for BMS internal update!\r\n");
        g_bat_update_ctx[6] = 0x12;
        break;

    case 0x12: /* terminal: release timer, signal result 1 */
        scheduler_release(g_bat_sched_slot + 4);
        g_bat_update_ctx[7] = 1;
        break;
    }

    return;
}


/* ════════════════════════════════════════════════════════════════════════════
 * Charge-policy lookup + charge/fault display leaves
 * ════════════════════════════════════════════════════════════════════════════
 */

/*
 * battery_charge_lookup @ 0x0803E314
 * Linear search of the 18-row charge-policy table (OEM flash 0x0804F47C) for the
 * row matching the pack's SoC, charge flags and temperature. A row matches when
 *   soc <= row.soc_max && (temp_delta <= row.temp_max || (row.flag_mask & flags))
 * (SoC compared unsigned, temperature signed, flags ORed). On a match it emits
 * the row's charge rate (-> out_rate) and mode (-> out_mode) and returns the row
 * index; returns -1 if no row matches. Driven by battery_charge_display_step.
 */
static const struct bat_charge_row {
    uint16_t soc_max;     /* +0 SoC ceiling (unsigned compare)               */
    int16_t  temp_max;    /* +2 temperature-delta ceiling (signed compare)   */
    uint16_t flag_mask;   /* +4 charge-flag override (ANDed against `flags`)  */
    uint16_t rate;        /* +6 -> out_rate (charge current limit)           */
    uint8_t  mode;        /* +8 -> out_mode (display/charge mode)            */
    uint8_t  _pad;        /* +9                                              */
} g_battery_charge_table[18] = {
    {   3, 32767, 0x0000,   0, 1, 0 }, {   5, 32767, 0x0000,   0, 2, 0 },
    {  13,     0, 0x4000, 100, 3, 0 }, {  18,     0, 0x4000, 100, 3, 0 },
    {  22,     0, 0x4000, 100, 3, 0 }, { 100,     0, 0x4000, 100, 5, 0 },
    {  13,    50, 0x0020, 100, 3, 0 }, {  18,    50, 0x0020, 100, 3, 0 },
    {  22,    50, 0x0020, 150, 3, 0 }, { 100,    50, 0x0020, 200, 5, 0 },
    {  13,   600, 0x0000, 100, 4, 0 }, {  18,   600, 0x0000, 150, 4, 0 },
    {  22,   600, 0x0000, 200, 4, 0 }, { 100,   600, 0x0000, 300, 6, 0 },
    {  13, 32767, 0x8000, 100, 3, 0 }, {  18, 32767, 0x8000, 100, 3, 0 },
    {  22, 32767, 0x8000, 150, 3, 0 }, { 100, 32767, 0x8000, 200, 5, 0 },
};

int battery_charge_lookup(uint32_t soc, uint16_t flags, int16_t temp_delta,
                          uint16_t *out_rate, char *out_mode)
{
    int i;

    for (i = 0; i <= 0x11; i++) {
        const struct bat_charge_row *e = &g_battery_charge_table[i];
        if ((soc <= e->soc_max) &&
            ((temp_delta <= e->temp_max) || ((e->flag_mask & flags) != 0))) {
            *out_rate = e->rate;
            *out_mode = (char)e->mode;
            return i;
        }
    }
    return -1;
}

/*
 * battery_charge_complete_watchdog @ 0x0803E414
 * Runs in the "full" charge policy (mode 6): a two-stage scheduler timeout
 * (2 s then 10 s, slots at G_BAT_STATE+1/+2) that forces the displayed charge
 * rate to the 0xFA sentinel and then releases both timer slots. Only arms while
 * target_rate is below -24999 (and not exactly -32000).
 */
void battery_charge_complete_watchdog(int target_rate, uint16_t *out_rate)
{
    uint8_t *bs = G_BAT_STATE;   /* timer slots: +1 (2 s), +2 (10 s) */

    if ((target_rate < -24999) && (target_rate != -32000) && (bs[1] == 0xFA)) {
        bs[1] = scheduler_alloc();
        scheduler_start(bs[1], 2000, (sched_cb_t)0);
    }
    if (scheduler_slot_is_idle(bs[1]) != 0) {
        *out_rate = 0xFA;
        if (bs[2] == 0xFA) {
            bs[2] = scheduler_alloc();
            scheduler_start(bs[2], 10000, (sched_cb_t)0);
        }
    }
    if (scheduler_slot_is_idle(bs[2]) != 0) {
        scheduler_release(&bs[1]);
        scheduler_release(&bs[2]);
    }
}

/*
 * battery_set_charge_mode @ 0x0803E4AC
 * Store the resolved charge-mode byte into the app/session context at +0x341 —
 * reached through the pointer held in the batteryware-update record
 * (*(uint8_t**)G_BATWARE_UPD). This is the field the BMS charge setpoint is
 * driven from.
 */
void battery_set_charge_mode(uint8_t mode)
{
    *(*(uint8_t **)G_BATWARE_UPD + 0x341) = mode;
}

/*
 * battery_telemetry_state_get @ 0x0803E5F0
 * Return the battery telemetry FSM state byte (G_BAT_STATE+3, the selector
 * battery_telemetry_step switches on). Read by status_process.
 */
uint8_t battery_telemetry_state_get(void)
{
    return G_BAT_STATE[3];
}

/*
 * battery_fault_warning_report @ 0x0803EA88
 * Fold the BMS fault word (ctx+0x3F6) into the 32-bit state-flags mirror
 * (ctx+0x3B8): 0xFFFF latches the global fault flag (state_flags_set 0x10000),
 * 0 clears the low 16 bits, else ORs the fault bits in. When the charger GPIO
 * (GPIOC PC4) is asserted the charge-enable bits (& ~0x3000) are cleared. Logs
 * "BMS fault"/"BMS warning" on each edge-change vs the last-seen cache in the
 * batteryware-update record (G_BATWARE_UPD+0x3A fault / +0x3C warning).
 * ctx == the app/session telemetry context.
 */
void battery_fault_warning_report(int ctx)
{
    uint16_t fault = *(uint16_t *)(ctx + 0x3f6);

    if (fault == 0xFFFF) {
        state_flags_set(0x10000, 0);
    } else if (fault == 0) {
        *(uint32_t *)(ctx + 0x3b8) &= 0xFFFF0000u;
    } else {
        *(uint32_t *)(ctx + 0x3b8) |= fault;
    }

    if (HAL_GPIO_ReadPin((void *)GPIOC_BASE, 0x10) != 0) {   /* PC4 charger present */
        *(uint32_t *)(ctx + 0x3b8) &= 0xFFFFCFFFu;
    }

    if (*(int16_t *)(ctx + 0x3f6) != *(int16_t *)(G_BATWARE_UPD + 0x3a)) {
        *(int16_t *)(G_BATWARE_UPD + 0x3a) = *(int16_t *)(ctx + 0x3f6);
        log_print_timestamp_prefix();
        g_log_func("BMS fault 0x%04X\r\n", *(uint16_t *)(ctx + 0x3f6));
    }
    if (*(int16_t *)(ctx + 0x442) != *(int16_t *)(G_BATWARE_UPD + 0x3c)) {
        *(int16_t *)(G_BATWARE_UPD + 0x3c) = *(int16_t *)(ctx + 0x442);
        log_print_timestamp_prefix();
        g_log_func("BMS warning 0x%04X\r\n", *(uint16_t *)(ctx + 0x442));
    }
}

/*
 * bus_queue_peek_status @ 0x08039DF8
 * Peek the head of the BMS bus ring (handle at g_bat_modbus_ctx+0xE74) without
 * consuming, returning its first 16-bit word — used by battery_state_process as
 * a "queue drained / no pending item" probe.
 */
uint16_t bus_queue_peek_status(void)
{
    uint16_t v[3];
    bus_queue_peek(*(void **)(g_bat_modbus_ctx + 0xE74), v);
    return v[0];
}

/*
 * status_clear_pulse_flag @ 0x0802AAD8
 * Clear the one-shot "BMS pulse" status flag at the status/clock block +0x7C
 * (states.c G_CLK @ 0x200001D8); called by battery_state_process.
 */
void status_clear_pulse_flag(void)
{
    G_STATUS_CLK[0x7c] = 0;
}


/* ── Inter-module "Manchester" announce decoder + TIM10 staging (cluster) ─── */

extern void announce_mark(int channel);          /* app.c */
extern void charge_time_estimate_reset(void);    /* states.c 0x0802E40C */

/* Battery warranty/state-of-transition (WST) code -> label. OEM 0x080330E4. */
const char *wst_status_to_string(uint32_t wst)
{
    switch (wst) {
    case 0:  return "WST_NONE";
    case 1:  return "WST_DISCHARGE_MODE";
    case 2:  return "WST_CHARGE_MODE";
    case 3:  return "WST_BYPASS_MODE";
    default: return "UNKNOWN";
    }
}

/* Decode one staged inter-module battery "announce" frame into the telemetry
 * cache and log it. Frame type byte msg[0]: 0 serial, 1 fw version, 2 SoC,
 * 3 SoH/NoC/WST (a 7-byte frame also carries the WST byte); anything else is
 * hex-dumped. OEM 0x08043B28. */
void manchester_announce_decode(const uint8_t *msg, int len, uint8_t *cache)
{
    switch (msg[0]) {
    case 0:                                          /* serial number */
        if (cache[0xd] == 0) {
            announce_mark(0);
        }
        cache[0xd] = 1;
        charge_time_estimate_reset();
        *(uint32_t *)(cache + 1) = *(const uint32_t *)(msg + 1);
        cache[5] = msg[5];
        log_print_timestamp_prefix();
        g_log_func("serial %02X.%02X.%02X.%02X.%02X\r\n",
                   cache[1], cache[2], cache[3], cache[4], cache[5]);
        break;
    case 1:                                          /* firmware version */
        if (cache[0xd] == 0) {
            announce_mark(0);
        }
        cache[0xd] = 1;
        charge_time_estimate_reset();
        cache[8] = msg[3];
        *(uint16_t *)(cache + 6) = *(const uint16_t *)(msg + 1);
        log_print_timestamp_prefix();
        g_log_func("version %02X.%02X.%02X\r\n", cache[6], cache[7], cache[8]);
        break;
    case 2:                                          /* state of charge */
        if (cache[0xd] == 0) {
            announce_mark(0);
        }
        cache[0xd] = 1;
        cache[0] = msg[1];
        charge_time_estimate_reset();
        log_print_timestamp_prefix();
        g_log_func("soc %d\r\n", (int8_t)cache[0]);   /* OEM ldrsb: SoC logged signed */
        break;
    case 3:                                          /* SoH / NoC / WST */
        if (cache[0xd] == 0) {
            announce_mark(0);
        }
        cache[0xd] = 1;
        cache[9] = msg[1];
        *(uint16_t *)(cache + 10) = *(const uint16_t *)(msg + 2);
        cache[0xc] = (len == 7) ? msg[4] : 0;
        charge_time_estimate_reset();
        log_print_timestamp_prefix();
        g_log_func("soh %d noc %d wst %s(%d)\r\n",
                   cache[9], *(uint16_t *)(cache + 10),
                   wst_status_to_string(cache[0xc]), cache[0xc]);
        break;
    default:
        g_log_func("Unsupported Manchester message\r\n");
        for (int i = 0; i < len; i++) {
            g_log_func("0x%02X ", msg[i]);
        }
        g_log_func("\r\n");
        break;
    }
}

/* Modbus CRC-16 (poly 0xA001, seed 0xFFFF) over the staged frame; the result is
 * returned in r0 and consumed by the caller. OEM 0x08043AF0. */
uint16_t staged_msg_crc16(const uint8_t *buf, int len)
{
    uint16_t crc = 0xFFFF;
    int i;

    for (i = 0; i < len; i++) {
        crc ^= buf[i];
        for (short bit = 8; bit != 0; bit--) {
            if ((crc & 1) == 0) {
                crc >>= 1;
            } else {
                crc = (uint16_t)((crc >> 1) ^ 0xA001);
            }
        }
    }
    return crc;
}

/* TIM10 period-elapsed callback (OEM 0x08043DE0): reload the counter and commit
 * the double-buffered staged announce record (pending -> active) one tick after
 * it was armed. Reached from HAL_TIM_IRQHandler for htim10 (handle 0x20009A04). */
void tim10_announce_period_cb(void *htim)
{
    volatile uint32_t *htim10 = (volatile uint32_t *)0x20009A04u;   /* HTIM10 */
    volatile uint8_t  *rec    = (volatile uint8_t *)0x200096D4u;    /* staged record */

    if (*(volatile uint32_t *)htim != 0x40014400u) {                /* not TIM10 */
        return;
    }
    /* TIM10->CNT = htim10.Init.Period - 0x682 */
    ((volatile uint32_t *)htim10[0])[0x24 / 4] = htim10[0xc / 4] - 0x682u;

    if (rec[0x24] != 0) {                            /* freshly armed -> mark pending */
        rec[0x25] = 1;
        rec[0x24] = 0;
        return;
    }
    if (rec[0x25] != 0) {                            /* pending -> commit to active */
        *(volatile uint32_t *)(rec + 4)    = *(volatile uint32_t *)(rec + 0x14);
        *(volatile uint32_t *)(rec + 8)    = *(volatile uint32_t *)(rec + 0x18);
        *(volatile uint32_t *)(rec + 0xc)  = *(volatile uint32_t *)(rec + 0x1c);
        *(volatile uint32_t *)(rec + 0x10) = *(volatile uint32_t *)(rec + 0x20);
        rec[1] = rec[0x26];
        rec[0] = 1;
        rec[0x25] = 0;
        return;
    }
    rec[0x25] = 0;
}
