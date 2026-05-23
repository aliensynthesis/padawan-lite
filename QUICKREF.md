# Padawan-Lite Quick Reference

For ITU-T X.28 Packet Assembler/Disassembler v1.1.

## Invocation

```
padawan-lite [-c FILE] [-l PORT] [-a FILE] [-t] [-h]
```

| Flag                       | Effect                                                   |
| -------------------------- | -------------------------------------------------------- |
| `-c`, `--config FILE`      | Address map: `<address> <host> <port>` per line          |
| `-l`, `--listen PORT`      | Multi-session TCP server (max 16 concurrent users)       |
| `-a`, `--auth FILE`        | Require NUI; FILE = one allowed NUI per line             |
| `-t`, `--telnet-defaults`  | Apply `SET 2:0, 3:0, 4:1` per session (char-at-a-time)   |
| `-b`, `--baud BPS`         | Throttle I/O to BPS bits/sec (300/1200/2400/…; 0 = off)  |
| `--trace`                  | Log inbound traffic per session (`hexdump -C` format)    |
| `--trace-prefix PREFIX`    | Override `--trace` filename prefix (implies `--trace`)   |
| `--trace-line-mode`        | CLIENT entries flush on CR; SERVICE pairs with each line |
| `--pcp-port PORT`          | PAD Control Protocol listener on `127.0.0.1:PORT`        |
| `-h`, `--help`             | Show usage                                               |

Default (no flags): one session bound to stdin/stdout, simple profile,
raw-mode tty if stdin is a terminal.

## PAD commands (X.28 §3.5)

Terminate each command with `<CR>` (or `+`). Commands valid in
PAD-command state (after recall, or before any call is up).

| Short    | Long          | Effect                                             |
| -------- | ------------- | -------------------------------------------------- |
| `STAT`   | `STATUS`      | Report `FREE` or `ENGAGED`                         |
| `CALL …` | `CALL …`      | Place a call (also: bare selection signal)         |
| `CLR`    | `CLEAR`       | Clear the active call                              |
| `ICLEAR` |               | Invitation to clear (X.29)                         |
| `INT`    | `INTERRUPT`   | Send X.25 interrupt to remote                      |
| `RESET`  |               | Reset the virtual call                             |
| `BREAK`  |               | Issue X.28 break (also via Ctrl-B in tty mode)     |
| `PAR?p…` |               | Read params: `PAR?2,3,4`                           |
| `SET p:v`|               | Write params: `SET 2:0, 3:0`                       |
| `READ`   | `RREAD`       | Read all params                                    |
| `SETREAD`| `RSETREAD`    | Set + read in one shot                             |
| `PROF n` | `PROFILE n`   | Load profile (simple=1, transparent=91)            |
| `ID nui` |               | Set session NUI (X.28 §5.2)                        |
| `IDOFF`  |               | Clear session NUI                                  |
| `LANG x` | `LANGUAGE x`  | Select response language (only English wired)      |
| `HELP`   |               | Show help text                                     |

## Selection signal syntax

```
[facilities-]address<CR>
```

Place a call to `12345`:
```
12345
```

With NUI `david` (required when `--auth` is on):
```
Ndavid-12345
```

Multiple facilities, comma-separated, dash before address:
```
Ndavid,R,G01-12345
```

Facility letters (Table 4/X.28):

| Code | Meaning                              |
| ---- | ------------------------------------ |
| `N`  | NUI                                  |
| `R`  | Reverse charging                     |
| `G`  | Closed user group (e.g. `G01`)       |
| `C`  | CUG with outgoing access             |
| `D`  | Throughput class                     |
| `F`  | Fast select                          |
| `Q`  | Charging information                 |
| `T`  | Transit delay                        |

Arg syntax checks are not enforced in v1.1 — strings pass through verbatim.

## X.3 parameters (most-used)

| #   | Name                | Simple | Transparent | Notes                  |
| --- | ------------------- | ------ | ----------- | ---------------------- |
| 1   | PAD recall char     | 1      | 0           | 1 = DLE (Ctrl-P)       |
| 2   | Echo                | 1      | 0           | 0=off, 1=on, 2=except  |
| 3   | Forwarding chars    | 126    | 0           | bitmask, see X.3 3.3   |
| 4   | Idle timer (×50 ms) | 0      | 20          | 0 = disabled           |
| 5   | DTE flow control    | 1      | 0           | XON/XOFF from DTE      |
| 6   | Service signals     | 1      | 0           | 0=none, 1=std, 5=ext   |
| 7   | Break action        | 2      | 21          | bitmask, X.3 3.7       |
| 9   | CR padding          | 0      | 0           | post-CR pad chars      |
| 10  | Line folding        | 0      | 0           | 0 = no fold            |
| 12  | DTE flow ctl (PAD)  | 1      | 0           | XON/XOFF to PAD        |
| 13  | LF after CR         | 0      | 0           | bits 0/1/2             |
| 14  | LF padding          | 0      | 0           | post-LF pad chars      |
| 15  | Editing             | 0      | 0           | 1 = enabled            |
| 16  | Char delete         | 8      | 127         | IA5 code for `cdel`    |
| 17  | Line delete         | 24     | 24          | IA5 code for `ldel`    |
| 18  | Line display        | 18     | 18          | IA5 code for `ldis`    |
| 22  | Page wait LF count  | 0      | 0           | 0 = disabled           |

`--telnet-defaults` is shorthand for `SET 2:0, 3:0, 4:1`.

## Control bytes (interactive tty only)

| Byte    | Action                          |
| ------- | ------------------------------- |
| Ctrl-P  | PAD recall (DLE, 0x10)          |
| Ctrl-B  | Break                           |
| Ctrl-D  | Quit (driver-level, not X.28)   |

In piped/non-tty stdin mode and TCP `--listen` mode, these intercepts
are disabled — every byte passes through transparently.

## Service signal causes (Table 5/6 / X.28)

Clear (`CLR <cause> C:n D:n`):

| Cause | Meaning                            |
| ----- | ---------------------------------- |
| `OCC` | Number busy                        |
| `NC`  | Network congestion                 |
| `INV` | Invalid facility request           |
| `NA`  | Access barred (auth gate)          |
| `ERR` | Local procedure error              |
| `RPE` | Remote procedure error             |
| `NP`  | Number not assigned                |
| `OOO` | Out of order                       |
| `DTE` | Cleared by remote DTE              |
| `DER` | Remote device error                |
| `RCH` | Reverse-charging refused           |
| `ID`  | Incompatible destination           |

Reset (`RESET <cause> D:n`):

| Cause | Meaning                  |
| ----- | ------------------------ |
| `DTE` | Reset by remote DTE      |
| `ERR` | Local procedure error    |
| `NC`  | Network congestion       |
| `RPE` | Remote procedure error   |

Other: `FREE`, `ENGAGED`, `COM`, `ERR`, `PAR p:v, …`, `PAGE`, `CLR CONF`.

## Auth file format

```
# Comments and blank lines ignored. One NUI per line.
# Max 64 entries, max 31 chars each.
david
alice
bob
```

`--auth FILE` makes every `SELECTION` require a matching `N<nui>` in the
facility block. Calls without `N`, or with a `N` that's not in the
allow-list, are rejected with `CLR NA C:0 D:0`. v1.1 does not yet
honour session-level `ID <nui>` as a fallback — see `deviations.txt`.

## Address map file format

```
# <address> <host> <port>   '#' comments / blank lines ignored.
30001 localhost 30001
12345 host.example.com 8023
```

Loaded via `--config FILE`. Max 32 entries; address ≤ 15 chars, host
≤ 63 chars; IPv4 only. Unmapped addresses fall back to
`localhost:<address-as-port>`.

## Example session

```
$ padawan-lite --auth nuis.txt --config addrs.txt --telnet-defaults --listen 30000
Padawan-Lite v1.1 - listening on TCP port 30000 (MAX_SESSIONS = 16).
NUI auth: 1 entries loaded; calls require a matching N facility.
Telnet-friendly defaults: SET 2:0, 3:0, 4:1 applied per session.
```

From another terminal:
```
$ telnet localhost 30000
<CR>                          # complete handshake
PADAWAN-LITE v1.1
Ndavid-30001<CR>              # call address 30001 with NUI=david
COM                           # connected
... data flows ...
<Ctrl-P>                      # PAD recall (if param 1 = 1)
CLR<CR>                       # clear the call
CLR CONF
```
