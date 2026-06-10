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

## Response handlers & command builders (the callback layer)

Each script entry can carry a `.build_cb` (format a custom outgoing command) and
a `.handle_cb` (post-process a matched reply). `modem_at_exec` calls them with
`pkt = {buf-or-response, bufsize, fmt}`; `handle_cb` returns `0` done-OK, `2`
alt-OK, `3` fail, `build_cb` returns `0`/`3`. They are sourced in `modem.c` on
top of four parsing primitives:

- `modem_skip_to_cr` / `modem_skip_to_space` (`0x0802F1DC` / `0x0802F1F0`) —
  advance past the next `\r` / `' '`.
- `modem_extract_field` (`0x0802F204`) — copy the next quoted/comma field
  (skips a leading `+CMD ` token and quotes); `dst == NULL` = locate-only.
- `modem_at_response_copy` (`0x0802F404`) — copy 0x80 bytes of the raw reply,
  CR→space, into the working buffer before matching.

| callback | OEM | AT cmd | what it does |
| --- | --- | --- | --- |
| `modem_parse_manufacturer` | `0x0802F36C` | `CGMI` | field → `g_modem_manufacturer` |
| `modem_parse_model` | `0x0802F338` | `CGMM` | field → `g_modem_model_resp` (SARA/LARA select) |
| `modem_parse_revision` | `0x0802F304` | `CGMR` | field → `g_modem_revision` |
| `modem_parse_imei` | `0x0802F2D0` | `CGSN` | field → `g_modem_imei` |
| `modem_parse_imsi` | `0x0802F29C` | `CIMI` | field → `g_modem_imsi` |
| `modem_handle_ccid` | `0x0802F494` | `CCID` | field → `g_modem_iccid` (the SIM-lock serial) |
| `modem_handle_cpin` | `0x0802F4D4` | `CPIN?` | `READY`→2, `SIM PIN`→0, else 3 |
| `modem_handle_csq` | `0x0802F5FC` | `CSQ` | RSSI → `g_modem_csq`; accept < 99 |
| `modem_handle_creg` | `0x0802F434` | `CREG?` | accept reg-state ∈ {1,5,6,7,9} |
| `modem_handle_cmgf` | `0x0802F524` | `CMGF?` | `0` PDU→0, `1` text→2 |
| `modem_handle_upsnd` | `0x0802F560` | `UPSND` | PSD activation flag (2nd field) |
| `modem_handle_uuhttpcr` | `0x0802F5B0` | `+UUHTTPCR` | HTTP result code 0/1 |
| `modem_handle_cpms` | `0x0802F644` | `CPMS?` | stored-SMS count → `g_modem_sms_used` |
| `modem_handle_cmgl` | `0x0802F684` | `CMGL` | SMS index → `g_modem_sms_index` (1..0x130) |
| `modem_handle_cmgr_sms` | `0x0802FC30` | `CMGR` | origin → `g_modem_sms_number`, body → SMS dispatcher |
| `modem_handle_ugsrv` | `0x0802FC84` | `UGSRV` | AssistNow server config check (host/token) |
| `modem_handle_ugaop` | `0x0802FD0C` | `UGAOP` | AssistNow aiding config check |
| `modem_handle_upsd` | `0x0802FD68` | `UPSD` | APN-match check vs `g_pModemProvision[1]` |
| `modem_handle_uuloc` | `0x0802F8D4` | `+UULOC` | build `imei=…&rmc=$…` into `g_modem_http_payload` |
| `modem_build_cpin` | `0x0802F6D8` | `CPIN=` | `%s` = SIM PIN (`g_pModemProvision[0]`) |
| `modem_build_cmgs` / `_sms_body` | `0x0802F7CC` / `0x0802F798` | `CMGS` | recipient `"%s"` then body `%s`+Ctrl-Z |
| `modem_build_cmgr` / `_cmgd` | `0x0802F748` / `0x0802F710` | `CMGR`/`CMGD` | read/delete SMS slot `%d` |
| `modem_build_upsd_apn` | `0x0802F800` | `UPSD=` | APN `%s` (`g_pModemProvision[1]`) |
| `modem_build_uhttp_port` | `0x0802F86C` | `UHTTP` | port `"443"` |
| `modem_build_uhttpc_payload` | `0x0802F838` | `UHTTPC` | body `%s` = `g_modem_http_payload` |
| `modem_build_ugaop` | `0x0802F980` | `UGAOP=` | host `ublox1.vanmoof.com`, port `46434` |

### Inbound SMS remote control

`modem_handle_cmgr_sms` hands the SMS body to **`modem_sms_dispatch_command`**
(`0x0803D668`): bodies of the form `#<8-char-code>*<cmd>` drive remote actions —
unlock / factory-reset / bike-state change / location report (formats the BLE MAC
+ counters and POSTs) / bell (`ssp_ble_enqueue_tx_packet`). This is the SMS half
of the anti-theft control surface (the BLE half is `ble_cmd_dispatch`). Mapped +
named; the dispatcher body itself is not yet sourced.

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

The response handlers first write into a block of fixed SRAM scratch buffers
(the identity strings are a descending 16-byte run; the response handlers fill
them, and the outer SM copies them into the session context):

| SRAM | global | filled by |
| --- | --- | --- |
| `0x20009CC0` | `g_modem_manufacturer` (16) | `CGMI` |
| `0x20009CD0` | `g_modem_model_resp` (16) | `CGMM` |
| `0x20009CE0` | `g_modem_revision` (16) | `CGMR` |
| `0x20009CF0` | `g_modem_imei` (16) | `CGSN` |
| `0x20009D00` | `g_modem_imsi` (16) | `CIMI` |
| `0x20009D10` | `g_modem_iccid` (0x15) | `CCID` |
| `0x20009D25` | `g_modem_csq` (3) | `CSQ` |
| `0x20009C0C` | `g_modem_sms_index` (u16) | `CMGL` (CMGR/CMGD operand) |
| `0x20009D28` | `g_modem_sms_read_count` (u8) | bounded CMGR read progress |
| `0x20009D8C` | `g_modem_sms_used` (u8) | `CPMS` stored count (read cap) |
| `0x20009D2C` | `g_modem_sms_number` | recipient (CMGS) / origin (CMGR) |
| `0x20009D3C` | `g_modem_sms_body` | SMS text (= number + 0x10) |
| `0x20009C20` | `g_modem_http_payload` (0x80) | UHTTPC request body |

Provisioning strings live in flash at `0x0804F440` (`g_pModemProvision`):
`[0]` = SIM PIN (`""`), `[1]` = APN (`m2m.vanmoof.com`).
