# mainware — debug console command table

UART debug console (login-gated by `login_handler`). Dispatcher matches the typed
token against a 49-entry table at flash `0x0804F5C4` (12-byte `{name, help,
handler}` entries). Sub-shells via UART redirect: `bledebug`→UART8 (CC2642),
`gsmdebug`→UART2 (u-blox modem AT), `bmsdebug`→Modbus; `shiftware`→shifter (sel
`0x2000019C`). Raw Modbus diagnostics: `b*`=battery (slave `0xAA`), `s*`=shifter
(slave `0x20`); funcs 0x03 read / 0x06 write-single / 0x10 write-multiple.

**All 49 handlers decoded** — 47 sourced into `console.c`, `show` + `ver`
named+documented (large logging dumps).

| # | command | handler | help | status |
|---|---------|---------|------|--------|
| 0 | `help` | `0x08040aa0` | This tekst | `console_cmd_*` (console.c) |
| 1 | `reboot` | `0x08041da4` | reboot CPU | `console_cmd_*` (console.c) |
| 2 | `login` | `0x080425f4` | Login shell | `console_cmd_*` (console.c) |
| 3 | `logout` | `0x08040a4c` | Logout shell | `console_cmd_*` (console.c) |
| 4 | `ver` | `0x08040ae4` | Software version | console_cmd_ver (named/documented) |
| 5 | `distance` | `0x08041360` | Manual set dst | `console_cmd_*` (console.c) |
| 6 | `gear` | `0x080425ac` | set gear | `console_cmd_*` (console.c) |
| 7 | `region` | `0x080421cc` | Region 0..3 | `console_cmd_*` (console.c) |
| 8 | `blereset` | `0x08041fb8` | hard reset BLE | `console_cmd_*` (console.c) |
| 9 | `bledebug` | `0x08040c6c` | redirect uart8 | `console_cmd_*` (console.c) |
| 10 | `show` | `0x08042714` | Parameters | console_cmd_show (named/documented) |
| 11 | `motorupdate` | `0x08042590` | Update F2806 CPU | `console_cmd_*` (console.c) |
| 12 | `vollow` | `0x080424a4` | Audio volume | `console_cmd_*` (console.c) |
| 13 | `volmid` | `0x080423b8` | Audio volume | `console_cmd_*` (console.c) |
| 14 | `volhigh` | `0x080422cc` | Audio volume | `console_cmd_*` (console.c) |
| 15 | `wheelsize` | `0x08042120` | Wheel 24/28 inch | `console_cmd_*` (console.c) |
| 16 | `speed` | `0x0804131c` | override speed | `console_cmd_*` (console.c) |
| 17 | `loop` | `0x08040c28` | main loop time | `console_cmd_*` (console.c) |
| 18 | `shipping` | `0x080415d0` | Shipping mode | `console_cmd_*` (console.c) |
| 19 | `factory-shipping` | `0x08041ff8` | Factory shipping mode (ignores BMS) | `console_cmd_*` (console.c) |
| 20 | `logprn` | `0x08041f88` | Print log | `console_cmd_*` (console.c) |
| 21 | `logclr` | `0x08041f34` | Clear log 6 | `console_cmd_*` (console.c) |
| 22 | `logapp` | `0x08041e94` | 1/ 0 | `console_cmd_*` (console.c) |
| 23 | `powerchange` | `0x080412bc` | 1/ 0 | `console_cmd_*` (console.c) |
| 24 | `factory` | `0x08041e70` | Load factory defaults | `console_cmd_*` (console.c) |
| 25 | `battery` | `0x08042f28` | Show battery | `console_cmd_*` (console.c) |
| 26 | `batware` | `0x08041e50` | Battery update | `console_cmd_*` (console.c) |
| 27 | `batreset` | `0x08041dd8` | Battery reset | `console_cmd_*` (console.c) |
| 28 | `shiftware` | `0x08041dbc` | Battery update | `console_cmd_*` (console.c) |
| 29 | `shifterstatus` | `0x08042f74` | Show shifter | `console_cmd_*` (console.c) |
| 30 | `shiftdebug` | `0x08041d50` | Show Modbus | `console_cmd_*` (console.c) |
| 31 | `shiftresetcounter` | `0x08040cb4` | Reset shift counter | `console_cmd_*` (console.c) |
| 32 | `motorstatus` | `0x08042e54` |  | `console_cmd_*` (console.c) |
| 33 | `gsminfo` | `0x08040d14` | Info from Ublox | `console_cmd_*` (console.c) |
| 34 | `gsmstart` | `0x08041d38` | start GSM function | `console_cmd_*` (console.c) |
| 35 | `gsmdebug` | `0x08040c90` | redirect uart2 | `console_cmd_*` (console.c) |
| 36 | `bmsdebug` | `0x08040cd8` | Show Modbus | `console_cmd_*` (console.c) |
| 37 | `sound` | `0x08041d10` | sample,volume,times | `console_cmd_*` (console.c) |
| 38 | `adc` | `0x08043028` | read adc | `console_cmd_*` (console.c) |
| 39 | `bwritereg` | `0x08041c84` | Modbus Bat write register | `console_cmd_*` (console.c) |
| 40 | `bwritedata` | `0x08041bb4` | Modbus Bat write data | `console_cmd_*` (console.c) |
| 41 | `breadreg` | `0x08041b30` | Modbus Bat read register | `console_cmd_*` (console.c) |
| 42 | `swritereg` | `0x08041528` | Modbus Shift write register | `console_cmd_*` (console.c) |
| 43 | `swritedata` | `0x0804168c` | Modbus Shift write data | `console_cmd_*` (console.c) |
| 44 | `sreadreg` | `0x080414a4` | Modbus Shift read register | `console_cmd_*` (console.c) |
| 45 | `stc` | `0x08041614` | read lipo monitor | `console_cmd_*` (console.c) |
| 46 | `stcreset` | `0x080415ec` |  | `console_cmd_*` (console.c) |
| 47 | `setoad` | `0x080415b4` | test | `console_cmd_*` (console.c) |
| 48 | `setgear` | `0x080413b4` | save muco shifter | `console_cmd_*` (console.c) |
