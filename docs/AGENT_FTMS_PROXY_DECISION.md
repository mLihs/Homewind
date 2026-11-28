# FTMS trainer proxy — architecture decision (#13)

Date: 2026-11-27  
Status: **DECIDED — Park (§2)** · maintainer OK 2026-11-27  
Parent: `AGENT_FEATURES.md` §2 · PRIORITIES #13 · after #4

## Context

| Path | What it does | Status |
| --- | --- | --- |
| **Trainer mirror** | Byte-identical GATT mirror of the real trainer (CPS/FE-C/FC82…). Apps control the trainer through the mirror. | **Shipped** Homewind 2.0+ / BBS 3.3.x (`AGENT_TRAINER_MIRROR.md`) |
| **§2 FTMS proxy** | Homewind exposes a synthetic **FTMS 0x1826** indoor bike to the app and translates Control Point ↔ trainer protocol. | **Not built** |

Zwift on NEO already gets control via the **mirror** (FC82), not via FTMS. §2 mainly helps **FTMS-native trainers** (Flux, many Elite/Wahoo with FTMS, Van Rysel D100, …) when you want one Homewind BLE slot *and* apps that only speak FTMS — or when mirroring that trainer’s full GATT is undesirable.

## Decision questions

1. **Do we still want §2** given the mirror covers the maintainer’s NEO + Zwift path?
2. **Where does FTMS code live** if yes?
3. **Discovery / roles** — keep HR/CSC/PWR slots, or move to role-based setup (power/cadence/control sources)?

## Options (where FTMS lives)

| | A — Inside BBS | B — Separate library | C — Park §2 |
| --- | --- | --- | --- |
| Shape | `BBS_COMPILE_FTMS=1` client+server next to trainer mirror | e.g. `FtmsProxy` / `BikeFtmsAsync`, Homewind wires beside BBS | No FTMS work; mirror + docs only |
| Pros | One BLE owner; shared NimBLE/port patterns | BBS stays focused; mirror budget already tight (~4 050 lines); clearer compile gates | Zero RAM/flash risk; matches current hardware story |
| Cons | Another large budget raise; conflicts conceptually with “mirror unchanged” | Two BLE stacks to coordinate (who owns advertising / connections) | FTMS-only trainers stay second-class unless used direct-to-app |
| Heap | High (second GATT + client) | High, but can stay off Basic | None |

## Decision

**C — Park §2.** Confirmed 2026-11-27.

Reasons still hold with a second trainer on the list: the mirror already covers
**Tacx NEO 2T** (maintainer) and **Wahoo KICKR Core** (user/Rouvy field report).
FTMS translation remains a multi-week heap-sensitive project with little gain
for current mirror users; P2 Dyson and other open items stay higher leverage.

**If §2 is resurrected later:** prefer **B (separate library)** so BBS mirror
and FTMS translation do not share one mega-module. Keep role/slot migration out
of the first FTMS slice.
