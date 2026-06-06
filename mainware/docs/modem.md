# mainware — cellular modem (u-blox SARA-G350) & the SIM lock

The bike "phones home" over a **u-blox SARA-G350** 2G/GPRS cellular module
(confirmed by the `"Poweron g350"` / `"Poweroff g350"` log strings; the firmware
also probes for `"SARA"` vs `"LARA"` so the same code base can drive a LARA LTE
module). The module hangs off a UART; everything below is driven from
`src/modem.c` + `modem_sim_state_machine`.

This is the subsystem behind VanMoof's tracking / anti-theft "find my bike"
backend, and behind the **SIM lock** that rejects non-VanMoof SIM cards.

## Two-level state machine

```
modem_sim_state_machine (0x0803D284)   outer SM, one tick / super-loop
   selects a high-level state ───────────────►  modem_step_<state>()   sub-SM, walks a list of AT cmds
                                                    └────►  modem_at_exec()  send 1 AT cmd, await reply, retry
```

**High-level states** (`modem_sm_state_name`, the byte the outer SM runs on):

| code | name | step function | what it does |
| --- | --- | --- | --- |
| 0 | `IDLE` | — | parked |
| 1 | `POWERON` | `modem_step_poweron` | power rails + PWR_KEY pulse, then the AT init handshake (below). The return code carries the discovered SIM/CPIN status. |
| 2 | `SMS_INIT` | `modem_step_sms_init` | SMS text-mode / storage setup |
| 3 | `SMS_READ` | `modem_step_sms_read` | list/read inbound SMS |
| 4 | `SMS_WRITE` | `modem_step_sms_write` | send SMS |
| 5 | `CTX_ACT` | `modem_step_ctx_activate` | bring the PDP/HTTP data context up; builds the HTTP body (MAC + `imei=…&rmc=…`) |
| 6 | `CTX_DEACT` | `modem_step_ctx_deactivate` | tear the PDP context down |
| 7 | `PING_SEND` | `modem_step_ping_send` | keep-alive |
| 8 | `MESSAGE_SEND` | `modem_step_message_send` | **HTTPS POST `/bike-message`** |
| 9 | `LOCATION_SEND` | `modem_step_location_send` | cell-location report |
| 10 | `POWEROFF` | `modem_step_poweroff` | AT shutdown, wait Vgsm<200 mV, drop rails → then `sim_iccid_check` |

The outer `modem_sim_state_machine` is the orchestrator (named + documented, not
sourced — like the BLE dispatchers): it gates on SIM-detect (PE10) and the modem
info, sequences the states above, builds the HTTP request body in the `CTX_ACT`
phase (`snprintf` of the BLE MAC `ctx+0x390..0x395` and `imei=%s&rmc=$`), and
recycles through `POWEROFF` → `sim_iccid_check`.

## The AT-command engine

`modem_at_exec` (`0x0802F9BC`) is the bottom layer — a 3-phase primitive:

1. **idle** — arm the response timer, seed the retry count;
2. **send** — once the guard timer expires, `snprintf` the command into the tx
   scratch buffer (or call a custom builder callback) and flush it to the UART;
3. **wait** — drain rx bytes into a line buffer and match the expected reply
   (`modem_at_response_match`, which finds the last `\r\n…\r\n` line and compares
   it). On match, optionally run a response-handler callback.

Return: `1` busy/in-progress · `0` done-OK · `2/3` retry/fail. The per-command
parameters (format string, expected reply, timeouts, retry count, callbacks)
live in a **flash script table** (`modem_at_entry_t`, 0x20 bytes each, at OEM
`0x08043EDC`) carved into per-state arrays at fixed byte offsets.

## AT command map

The POWERON init handshake (the AT strings at OEM `0x08050C20`):

| # | command | purpose |
| --- | --- | --- |
| 0 | `AT` | autobaud / liveness |
| 1 | `AT+CGMI` | manufacturer |
| 2 | `AT+CGMM` | **model** (matched vs "SARA"/"LARA" to pick the init length: SARA→13, LARA→14 cmds) |
| 3 | `AT+CGMR` | firmware revision |
| 4 | `AT+CGSN` | **IMEI** |
| 5 | `AT+CPIN?` | SIM PIN state (→ status code to the outer SM) |
| 6 | `AT+CPIN="%s"` | enter SIM PIN (the alternate/retry table) |
| 7 | `AT+CIMI` | **IMSI** (subscriber identity) |
| 8 | `AT+CCID` | **ICCID** (SIM serial — the value the SIM lock checks) |
| 9 | `AT+CSQ` | signal quality |
| 10 | `AT+CREG?` | 2G network registration |
| 11 | `AT+COPS?` | operator |
| 12 | `AT+CEREG?` | LTE/EPS registration (LARA only) |

The data path (CTX_ACT → MESSAGE_SEND) uses the u-blox HTTP AT set:

```
AT+UHTTP=0,1,"<host>"                       set server hostname
AT+UHTTP=0,5,<port>                         set port
AT+UHTTP=0,6,1                              enable TLS/HTTPS
AT+UHTTPC=0,5,"/upload","https","<body>",1                          POST /upload
AT+UHTTPC=0,5,"/bike-message","https","{<json>}",6,"application/json"   POST /bike-message
```

Response URCs parsed: `+CREG:`, `+CPIN:`, `+CSQ:`, `+UUHTTPCR:` (HTTP result).

**Backend hosts** (rodata): `bikecomm.vanmoof.com`, `ublox1.vanmoof.com`,
`m2m.vanmoof.com` (the APN). See also `net.c` (the cloud request builders).

## The SIM lock — ICCID / Vodafone-NL check

`sim_iccid_check` (`0x0802E328`), run at the POWEROFF recycle, is VanMoof's
**SIM lock**. After printing the firmware version and the PDP counters
(`PDOCP %d` / `PDSCP %d`, from `ctx+0x498`/`+0x49A`), once the modem info is
ready it does:

```c
bounded_strncmp(iccid /* AT+CCID result */, "8931440400", 10) != 0
   → async_request_post(0, 0x1000000)          // raise a "wrong SIM" bus event
   → log "Wrong iccid, <sim|no sim>"            // SIM-present = PE10 (GPIOE pin10)
log "iccid <value>"                              // always log the actual ICCID
```

So the bike only accepts SIMs whose **ICCID begins `8931440400`**. Decoding that
prefix per ITU-T E.118: `89` = telecom, `31` = **Netherlands**, then the
VanMoof **Vodafone-NL** M2M batch — i.e. the SIM that ships inside the bike. Swap
in any other SIM and you get `Wrong iccid` and a bus event.

Note on terminology: the enforced comparison is on the **ICCID** (the SIM's
printed serial, read by `AT+CCID`), *not* the IMSI. The **IMSI** is read too
(`AT+CIMI`) and reported, but there is **no IMSI-prefix constant** in the image —
there is no `"20404"` (Vodafone-NL MCC+MNC) string and no IMSI comparison. The
"Vodafone NL prefix" lock is the ICCID `8931440400` check. (The `gsminfo` console
command separately prints `imei` / `imsi` / `iccid` / `csq` and a
`SIM: Holder` / `SIM: PCB` line — that Holder/PCB string belongs to that console
dump, not to `sim_iccid_check`, which logs `sim` / `no sim`.)

## Hardware — modem power & control GPIO

| signal | pin | drive | notes |
| --- | --- | --- | --- |
| main supply enable | **PB4** | 1 = on | dropped at POWEROFF case 3 |
| level-shifter enable | **PA15** | 1 = on | |
| reset | **PE6** | 1 = held in reset | deasserted (0) at power-on |
| PWR_KEY | **PB0** | pulse | held low, pulsed high ~150 ms to toggle the module |
| aux | **PB1** | 0 | held low |
| SIM-detect | **PE10** | input | 1 = SIM present (used by `sim_iccid_check`) |
| Vgsm sense | ADC | `adc_read_vgsm()` | POWEROFF waits until < 200 mV |

The modem timer slots live at SRAM `0x20000070` (`[0]`/`[1]` AT send/response
timers, reused by POWERON; `[2]` = the AT-init command count). The modem working
context (AT engine state, tx scratch buffer at `+0x18`, and a per-step
`(substate,substep)` pair for each `modem_step_*`) is one struct at SRAM
`0x20000294`. The parsed IMEI/IMSI/ICCID/CSQ land in the session context
(`ctx+0x3E8` modem-info block; ICCID at `+0x50`).
