/* motorware field-oriented control — how the motor is actually driven
 * (S.0.00.22). Reconstructed from the IDA C28x disassembly (FOC loop around
 * 0x3F1376; FAST-observer call at 0x3F16C1). Fixed-point IQ24 throughout.
 *
 * This is a sensorless InstaSPIN-FOC drive. The closed-source **FAST** flux/
 * angle/speed observer runs from on-chip ROM (the EST_* calls); the Clarke /
 * Park / PI / inverse-Park / SVGEN transforms are motorware's own IQ24 code,
 * inlined in flash (they are shown here as their MotorWare-equivalent calls —
 * the OEM compiles these `static inline`). The loop is paced by the ePWM4 tick
 * (sub_3F2826, counter 0x9009); a tick divider gates the slower speed loop.
 *
 * Pipeline (verified call order at 0x3F16C1+):
 *   ADC -> Iab -> EST_run -> getIab/getAngle/getFm
 *       -> doSpeedCtrl/doCurrentCtrl -> Park -> PI(Id,Iq) -> iPark
 *       -> getOneOverDcBus -> SVGEN -> ePWM1/2/3 compare.
 *
 * Without TI's cl2000 this documents the reconstruction; not yet linked.
 */
#include <stdint.h>
#include "motor_state.h"

typedef int32_t _iq;                       /* IQ24 fixed-point */
typedef struct { _iq value[2]; } iq_vec2;  /* (alpha,beta) or (d,q) */

typedef struct est_obj  est_obj_t;         /* opaque FAST observer (ROM/RAM) */
typedef struct ctrl_obj ctrl_obj_t;
typedef struct epwm_regs epwm_regs_t;

extern ctrl_obj_t *g_ctrl;                 /* 0x9024 */

/* --- FAST observer (closed-source, on-chip ROM) ------------------------- */
void  EST_run(est_obj_t *est, const iq_vec2 *Iab, const iq_vec2 *Vab,
              _iq dcBus, _iq spd_ref);     /* ROM 0x3F9468 */
void  EST_getIab_pu(est_obj_t *est, iq_vec2 *Iab);          /* 0x3F983C */
_iq   EST_getAngle_pu(est_obj_t *est);     /* ROM 0x3F8DFA — rotor elec. angle */
_iq   EST_getFm_pu(est_obj_t *est);        /* ROM 0x3F987B — mech. frequency */
uint16_t EST_doSpeedCtrl(est_obj_t *est);  /* run speed loop this tick? */
uint16_t EST_doCurrentCtrl(est_obj_t *est);/* run current loop this tick? */
_iq   EST_getOneOverDcBus_pu(est_obj_t *est);     /* 1 / Vdc */
_iq   EST_updateId_ref_pu(est_obj_t *est, _iq *Id_ref);   /* field weakening */
void  EST_genOutputLimits_Pid_Id(est_obj_t *est, void *pid);

/* --- transforms (inlined IQ24 in the OEM; shown as their roles) ---------- */
void  CLARKE_run(const _iq Iabc[3], iq_vec2 *Iab);
void  PARK_run(const iq_vec2 *Iab, _iq sin, _iq cos, iq_vec2 *Idq);
_iq   PID_run(void *pid, _iq ref, _iq fb);          /* Id / Iq / speed PI */
void  IPARK_run(const iq_vec2 *Vdq, _iq sin, _iq cos, iq_vec2 *Vab);
void  SVGEN_run(const iq_vec2 *Vab, _iq dutyAbc[3]); /* space-vector PWM */
void  iq_sincos(_iq angle, _iq *sin, _iq *cos);

/* --- the control object (live FOC state; CTRL_Obj in L3 RAM @0x9024) ----- */
struct ctrl_obj {
    est_obj_t *est;
    void  *pid_Id, *pid_Iq, *pid_spd;       /* the three PI controllers */
    iq_vec2 Iab, Idq, Vdq, Vab;
    _iq    Id_ref, Iq_ref, spd_ref, angle, speed_fb;
    epwm_regs_t *pwm[3];                     /* the three phase ePWMs */
    _iq    adc_offset[3], current_sf;        /* current sense offset & scale */
};

/* read one raw phase current from the ADC result and convert to pu IQ24:
   Iab[p] = (raw_adc[p] - offset[p]) * current_scale   (0x3F16A7 sequence). */
static _iq adc_to_current(int16_t raw, _iq offset, _iq sf)
{
    return ((_iq)(raw - offset) * (sf >> 8)) >> 16;     /* IQ multiply */
}

/* ===================================================================== */
/* The per-cycle FOC current loop (0x3F1376; EST_run @0x3F16C1).          */
/* Runs every ePWM4 tick: measure -> estimate -> control -> modulate.     */
/* ===================================================================== */
void foc_run(ctrl_obj_t *c, const int16_t adc[4], _iq dcBus)
{
    _iq sin, cos, dutyAbc[3];

    /* 1. ADC -> phase currents (alpha/beta) */
    c->Iab.value[0] = adc_to_current(adc[0], c->adc_offset[0], c->current_sf);
    c->Iab.value[1] = adc_to_current(adc[1], c->adc_offset[1], c->current_sf);

    /* 2. FAST sensorless observer -> flux / angle / speed */
    EST_run(c->est, &c->Iab, &c->Vab, dcBus, c->spd_ref);
    EST_getIab_pu(c->est, &c->Iab);
    c->angle    = EST_getAngle_pu(c->est);          /* rotor electrical angle */
    iq_sincos(c->angle, &sin, &cos);

    /* 3. speed loop (gated to the slower tick) -> Iq reference */
    if (EST_doSpeedCtrl(c->est)) {
        c->speed_fb = EST_getFm_pu(c->est);
        c->Iq_ref   = PID_run(c->pid_spd, c->spd_ref, c->speed_fb);
    }

    /* 4. current loop: Park -> Id/Iq PI -> field weakening -> iPark */
    if (EST_doCurrentCtrl(c->est)) {
        PARK_run(&c->Iab, sin, cos, &c->Idq);
        EST_updateId_ref_pu(c->est, &c->Id_ref);    /* field-weakening Id* */
        EST_genOutputLimits_Pid_Id(c->est, c->pid_Id);
        c->Vdq.value[0] = PID_run(c->pid_Id, c->Id_ref, c->Idq.value[0]);
        c->Vdq.value[1] = PID_run(c->pid_Iq, c->Iq_ref, c->Idq.value[1]);
        IPARK_run(&c->Vdq, sin, cos, &c->Vab);
    }

    /* 5. SVGEN (normalised by 1/Vdc) -> three ePWM phase duties */
    {
        _iq k = EST_getOneOverDcBus_pu(c->est);
        iq_vec2 Vab_pu = { { (c->Vab.value[0] * (k >> 8)) >> 16,
                             (c->Vab.value[1] * (k >> 8)) >> 16 } };
        SVGEN_run(&Vab_pu, dutyAbc);
    }

    /* 6. write the phase compares (ePWM CMPA, register offset 9) */
    *(volatile uint16_t *)((uintptr_t)c->pwm[0] + 9) = (uint16_t)dutyAbc[0];
    *(volatile uint16_t *)((uintptr_t)c->pwm[1] + 9) = (uint16_t)dutyAbc[1];
    *(volatile uint16_t *)((uintptr_t)c->pwm[2] + 9) = (uint16_t)dutyAbc[2];
}
