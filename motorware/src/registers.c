/* motorware register protocol — how mainware reads telemetry and commands the
 * motor (S.0.00.22). Reconstructed from the IDA C28x disassembly; each function
 * cites its flash address. This is the application layer on top of the SLIP
 * link in comm.c.
 *
 * A decoded request frame (CRC stripped, de-stuffed) is:
 *     frame[0] = type
 *     frame[1] = opcode    5 = ack, 6 = read register, 7 = write register
 *     frame[2..] = operands (register id, address, value)
 *
 * Reads enqueue a response {echoed id, word-count, data...}; the link layer
 * retransmits it until mainware acks (opcode 5). See docs/protocol.md.
 *
 * C28x: char/int are 16 bit; values cross the wire big-endian byte pairs.
 * Without TI's cl2000 this documents the reconstruction (not yet linked).
 */
#include <stdint.h>
#include "motor_state.h"

/* --- externs (defined elsewhere in the firmware) ------------------------ */
typedef struct hal_obj   hal_obj_t;       /* HAL object @ *(L3_HAL_OBJ) */
typedef struct ctrl_obj  ctrl_obj_t;      /* CTRL/EST object @ *(L3_CTRL_OBJ) */
extern hal_obj_t  *g_hal;                 /* 0x903E */
extern ctrl_obj_t *g_ctrl;                /* 0x9024 */

int      slip_rx_decode(uint8_t *rx);             /* sub_3F33E6: 0=frame ready,2=err */
void     slip_tx_frame(const uint8_t *buf, uint16_t len);  /* sub_3F3310 */
int      resp_enqueue(uint8_t id, uint8_t nwords, const uint16_t *buf); /* sub_3F32C9 */
void     resp_ack_clear(uint8_t id);              /* sub_3F345C: opcode-5 ack */
int16_t  EST_getSpeed_krpm(ctrl_obj_t *est);      /* ROM 0x3F96B3 — motor speed */
int16_t  speed_post(int16_t krpm);                /* sub_3EE471 */
int      hal_read_input(hal_obj_t *gpio, uint8_t pin);     /* sub_3F3B0C */
int32_t  cmd_value_to_iq(uint16_t raw);           /* sub_3F4BD9 + float→IQ chain */

/* L3 globals the handlers touch (word addresses, see motor_state.h) */
#define g_status_flags  (*(volatile uint16_t *)L3_STATUS_FLAGS)   /* 0x9017 */
#define g_enable_flag   (*(volatile uint16_t *)L3_ENABLE_FLAG)    /* 0x9013 */
#define g_mode          (*(volatile uint32_t *)L3_CTRL_919A)      /* 0x919A */

/* request/response framing constants */
enum { OP_ACK = 5, OP_READ = 6, OP_WRITE = 7, RESP_TYPE = 2 };

/* ===================================================================== */
/* 0x3EE47C — read register `id`, build a response, enqueue it.          */
/*   id 10 = firmware identity/version                                    */
/*   id 11 = digital input status bits                                    */
/*   id 12 = main telemetry: fault flags + MOTOR SPEED + 2 measurements   */
/*   id 13 = two scaled IQ measurements                                   */
/* ===================================================================== */
void read_register(uint8_t id)
{
    uint16_t buf[7];

    switch (id) {
    case 10:                                       /* identity / version */
        buf[0] = (*(volatile uint8_t *)0x90A4 << 8) | *(volatile uint8_t *)0x90A5;
        buf[1] = (*(volatile uint8_t *)0x90A2 << 8) | *(volatile uint8_t *)0x90A3;
        resp_enqueue(10, 2, buf);
        break;

    case 11: {                                     /* digital input bits */
        hal_obj_t *gpio = *(hal_obj_t **)((uintptr_t)g_hal + 8);
        buf[0] = 0;
        if (hal_read_input(gpio, 37)) buf[0] |= 1;
        if (hal_read_input(gpio, 32)) buf[0] |= 2;
        resp_enqueue(11, 1, buf);
        break;
    }

    case 12:                                        /* main telemetry block */
        buf[0] = g_status_flags;                    /* fault / status flags */
        /* speed is only valid once the estimator/controller is running */
        buf[1] = g_mode ? (uint16_t)speed_post(EST_getSpeed_krpm(g_ctrl)) : 0;
        buf[2] = *(volatile uint8_t *)L3_STAT_BYTE_18;
        buf[3] = *(volatile uint8_t *)L3_STAT_BYTE_19;
        buf[4] = *(volatile uint8_t *)L3_STAT_BYTE_1A;
        /* two fixed-point measurements, IQ→engineering units */
        buf[5] = (uint16_t)(((*(volatile int32_t *)L3_MEAS_9066 >> 4) * 625) >> 19);
        buf[6] = (uint16_t)(((*(volatile int32_t *)L3_MEAS_91D6 >> 4) * 625) >> 16);
        resp_enqueue(12, 7, buf);
        g_status_flags &= 0xFFFC;                   /* reading clears the low fault bits */
        break;

    case 13:                                        /* two scaled measurements */
        buf[0] = (uint16_t)(((*(volatile int32_t *)L3_MEAS_906A >> 4) * 125) >> 17);
        buf[1] = (uint16_t)(((*(volatile int32_t *)L3_MEAS_905C >> 4) << 2) >> 19);
        resp_enqueue(13, 2, buf);
        break;

    default:                                        /* unknown id: no response */
        break;
    }
}

/* ===================================================================== */
/* 0x3EE50E — write register `id` with the 16-bit value `raw` (frame      */
/* big-endian). Each setpoint is converted to IQ24 and stored where the   */
/* FOC loop reads it. id 20 = enable/mode flags; 21/23/24/25 = setpoints. */
/* ===================================================================== */
void write_register(uint8_t id, const uint8_t *operand)
{
    uint16_t raw0 = (operand[1] << 8) | operand[0];

    switch (id) {
    case 20:                                        /* enable / mode flags */
        if (operand[0] & 1) {                       /* request run */
            g_status_flags |= 0x8000;
            /* ... start action sub_3F40E8(150, …) ... */
        }
        g_enable_flag = (operand[1] >> 7) & 1;
        break;

    case 21:                                        /* setpoint A (scale 1/100) */
        *(volatile int32_t *)L3_CMD_SP_958C = cmd_value_to_iq(raw0);  /* ×0x42C8=100.0 */
        break;

    case 23:                                        /* setpoint group B */
        *(volatile int32_t *)0x907A         = cmd_value_to_iq(raw0);
        *(volatile int32_t *)L3_CMD_SP_958E = cmd_value_to_iq(raw0);
        *(volatile int32_t *)L3_CMD_SP_9590 = cmd_value_to_iq(raw0);
        break;

    case 24:                                        /* setpoint group C (scale 1/10) */
        *(volatile int32_t *)L3_CMD_SP_958E = cmd_value_to_iq(raw0);
        *(volatile int32_t *)L3_CMD_SP_9590 = cmd_value_to_iq(raw0);
        break;

    case 25:                                        /* setpoint D */
        *(volatile int32_t *)L3_SETPOINT_95A4 = cmd_value_to_iq(raw0);
        break;

    case 22:                                        /* ack / no-op */
    default:
        break;
    }
}

/* ===================================================================== */
/* 0x3F3472 — link service: decode one SLIP frame and dispatch the opcode.*/
/* Called from the main loop (sub_3EE894). This is the entry point for    */
/* every mainware command.                                                */
/* ===================================================================== */
void link_service(void)
{
    uint8_t *frame;
    int rc = slip_rx_decode((uint8_t *)L3_RX_FRAME_BUF);  /* 0x958A */

    if (rc == 2) {                                  /* receive error */
        g_status_flags |= 2;
        return;
    }
    if (rc != 0)                                    /* no complete frame yet */
        return;

    frame = *(uint8_t **)L3_RX_FRAME_PTR;           /* 0x95BA */
    switch (frame[1]) {                             /* opcode */
    case OP_READ: {                                 /* read register -> respond */
        uint8_t reg = frame[2] + frame[3];          /* (the operand byte pair) */
        uint16_t echo = frame[2];
        read_register(reg);
        { uint16_t r[3] = { RESP_TYPE, OP_ACK, echo }; slip_tx_frame((uint8_t *)r, 3); }
        break;
    }
    case OP_WRITE: {                                /* write register -> ack */
        uint16_t echo = frame[2];
        write_register(frame[2] + frame[3], frame + 4);
        { uint16_t r[3] = { RESP_TYPE, OP_ACK, echo }; slip_tx_frame((uint8_t *)r, 3); }
        break;
    }
    case OP_ACK:                                    /* mainware acked a queued response */
        resp_ack_clear(frame[2]);
        break;
    default:
        break;
    }
}
