# Mono MQ EMC command reference

This document describes the commands the mono EMC server currently supports.
Existing EMC clients for the old VxWorks based server should keep working with this backend.
This backend supports legacy systems and lives outside this repository. 
It is part of the HZB mono appplication and can also be reused for newer systems.

## Contents

1. [Connection and syntax](#1-connection-and-syntax)
2. [Replies and errors](#2-replies-and-errors)
3. [Commands](#3-commands)
   - [3.1 Energy and status](#31-energy-and-status)
   - [3.2 Sweep](#32-sweep)
   - [3.3 Parameters](#33-parameters)
   - [3.4 Undulator and measurement](#34-undulator-and-measurement)
   - [3.5 Connection check](#35-connection-check)
4. [Parameter names](#4-parameter-names)

---

## 1. Connection and syntax

- The server listens by default on TCP port **5001**
- Send one command per line: the command name, then its arguments separated
  by spaces, for example `SPE 400.0`.
- Command names are not case sensitive. Parameter names are.
- Each reply is one line, ended with a line feed by default. The IOC can switch
  this to CR+LF, CR or no terminator.

### Common server limits and idle timeout

The mono MQ backend uses the shared EMC server, so the listener address,
connection and request limits, and the idle timeout work as for every
backend. See [listener address, limits and timeouts](emc-limits-and-timeouts.md).

```text
emcBindAddress                  # show the listener address
emcBindAddress "192.168.2.40"  # listen only on this IOC address
emcIdleTimeout                  # show the idle timeout
emcIdleTimeout 0                # never close idle connections (default)
emcIdleTimeout 1800             # close after 30 minutes without traffic
```

For this backend: an idle timeout also counts time spent waiting for a mono
reply, and closing a connection does not cancel mono work already started.

## 2. Replies and errors

| Reply | Meaning |
| --- | --- |
| `t` | Success. |
| `t <value>` | Success with a value. Floats have six decimals, for example `t 400.000000`. |
| `f` | Failure: unknown command, missing or invalid argument, or the mono reported an error. |

`GLE` is the exception: it returns the error text without `t`.

After an `f`, read the error text with `GLE`.

### Status word (GST)

`GST` returns `t 0` when everything is ready. Otherwise the value is a sum of
these bits:

| Value | Meaning |
| ---: | --- |
| 1 | Mono moving, or sweep moving to its start position |
| 2 | Sweep running between start and end energy |
| 4 | Insertion device moving |
| 8 | Mono not initialised |
| 16 | Error active |
| 32 | Component or status not available |
| 64 | Limit switch reached |
| 128 | Previous operation still running |
| 256 | Stop in progress |
| 524288 | Insertion device in local mode |

When the exit slit translation follows the energy (parameter `TES_0_FOLLOW`),
its motion is included in the status.

---

## 3. Commands

### 3.1 Energy and status

| Command | Argument | Reply | Description |
| --- | --- | --- | --- |
| `GST` | – | `t <integer>` | Status word, see [Status word](#status-word-gst). |
| `STO` | – | `t` | Stop all mono motion. Replies at once; poll `GST` to see when the mono stopped. |
| `SPE` | energy, float (eV) | `t` | Move to energy. Poll `GST` to see when the move is done. |
| `GPE` | – | `t <float>` (eV) | Current energy. |
| `SPO` | wavelength, float (nm) | `t` | Move to wavelength. |
| `GPO` | – | `t <float>` (nm) | Current wavelength. |
| `GLE` | error number, integer ≥ 0 (optional, default 0) | text | Text of a logged error. An unused slot returns an empty line. |
| `OPN`, `CLO` | – | `t` | Accepted for compatibility; they do nothing. |

### 3.2 Sweep

Typical sequence: set start, end and velocity with `SSS`, `SSE`, `SSV`; send
`SI`; poll `GST` until it returns `t 0`; send `SR`. Watch `GPE` or `GPO` to see
when the end energy is reached.

| Command | Argument | Reply | Description |
| --- | --- | --- | --- |
| `SSS` | float (eV) | `t` | Sweep start energy (must be within the energy limits). |
| `SSE` | float (eV) | `t` | Sweep end energy (must be within the energy limits). |
| `SSV` | float (eV/s) | `t` | Sweep velocity. |
| `SGS` | – | `t <float>` | Sweep start energy. |
| `SGE` | – | `t <float>` | Sweep end energy. |
| `SGV` | – | `t <float>` | Sweep velocity. |
| `SI` | – | `t` | Sweep init: move to the start position. |
| `SR` | – | `t` | Sweep run. |

### 3.3 Parameters

Parameters are addressed by name; the names are listed in
[section 4](#4-parameter-names). The value can follow the name directly
(`SPD cff 2.25`) or after an `=` (`SPD cff =2.25`).

| Command | Argument | Reply | Description |
| --- | --- | --- | --- |
| `SPL` | name, integer | `t` | Set an integer parameter. |
| `SPD` | name, float | `t` | Set a float parameter. |
| `SPS` | name, string | `t` | Set a string parameter. |
| `GPL` | name | `t <integer>` | Read an integer parameter. |
| `GPD` | name | `t <float>` | Read a float parameter. The name may be in quotes. |

### 3.4 Undulator and measurement

| Command | Argument | Reply | Description |
| --- | --- | --- | --- |
| `UGG` | – | `t <float>` (mm) | Undulator gap. |
| `USG` | float (mm) | `t` | Move the undulator gap. |
| `DMEAS` | none, a number, or a float parameter name | `t <float>` | Without an argument or with a number: the ring current. With a name: the value of that float parameter, like `GPD`. |

### 3.5 Connection check

| Command | Argument | Reply | Description |
| --- | --- | --- | --- |
| `ping`, `health`, `test` | – | `t` | Check that the server answers. |

---

## 4. Parameter names

Names are case sensitive. `<n>` is an index: a number, or `L` (= 0) or `R`
(= 1). Columns **Set** and **Get** show whether a name can be written and read.
Float parameters can also be read with `DMEAS`.

### 4.1 Float parameters (`SPD` / `GPD` / `DMEAS`)

#### Mono, sweep and machine

| Name | Set | Get | Description |
| --- | :-: | :-: | --- |
| `minEnergy`, `maxEnergy` | | ✓ | Energy limits (eV). |
| `lastEnergy` | | ✓ | Last set energy (eV). |
| `GZeroOrderStep` | ✓ | ✓ | Zero-order step (arcsec). |
| `RingCurrent` | ✓ | ✓ | Ring current (readable even when the mono is not running). |
| `Ph_SollEnergie` | ✓ | ✓ | Target photon energy. |
| `sweepStart`, `sweepEnd` | ✓ | ✓ | Sweep start/end (eV). |
| `sweepVel` | ✓ | ✓ | Sweep velocity (eV/s). |
| `sweepOffset` | ✓ | ✓ | Sweep offset. |
| `sweepVminFak`, `sweepVmaxFak` | ✓ | ✓ | Velocity factors. |
| `sweepAvgGVel`, `sweepAvgSVel` | | ✓ | Average ID gap/shift velocity during the sweep. |
| `sweepMaxVelUnd` | | ✓ | Maximum undulator velocity. |
| `sweepRampStart`, `sweepRampEnd` | | ✓ | Ramp start/end points. |
| `HeyStart`, `HeyStop`, `HeyOffset` | ✓ | ✓ | Heydemann correction range and offset. |
| `AmcFactor` | ✓ | ✓ | AMC factor. |
| `touchDiff`, `touchDiffStop` | | ✓ | Touch difference (arcsec). |
| `Mup`, `Mlow`, `Gup`, `Glow` | | ✓ | Mirror/grating angle limits. |
| `DE_S`, `DE_G`, `DE_M`, `DE_CR` | | ✓ | Bandwidth contributions (slit, grating, mirror, crystal) (calculated by the mono). |
| `Gamma` | | ✓ | Gamma (calculated). |

#### Insertion device

| Name | Set | Get | Description |
| --- | :-: | :-: | --- |
| `undGap`, `undShift` | ✓ | ✓ | ID gap/shift (setting moves the ID). |
| `IdVelocity` | ✓ | ✓ | Gap velocity. |
| `IdSpeed` | | ✓ | ID speed. |
| `IdOffset`, `IdSlope` | ✓ | ✓ | Offset/slope of the current ID table. |
| `IdOffset<n>`, `IdSlope<n>` | ✓ | ✓ | Offset/slope of ID table n (from 1); saved when set. |

#### Grating and optics (active grating, current branch)

| Name | Set | Get | Description |
| --- | :-: | :-: | --- |
| `cff` | ✓ | ✓ | cff (must be ≥ 0 and ≠ 1). |
| `mirrorAngleAtZero`, `gratingAngleAtZero` | ✓ | ✓ | Angles at zero. |
| `CR1Offset`, `CTOffset` | ✓ | ✓ | Crystal offsets (DCM). |
| `GratingStepsAtZero` | ✓ | ✓ | Grating steps at zero. |
| `lineDensity` | ✓ | ✓ | Line density (lines/mm). |
| `d_hkl` | ✓ | ✓ | Crystal d spacing (nm). |
| `RG` | ✓ | ✓ | Grating radius. |
| `GZeroOrderPos` | ✓ | ✓ | Saved zero-order position. |
| `blazeAngle` | ✓ | ✓ | Blaze angle. |
| `FixBetaAngle`, `FixThetaAngle` | ✓ | ✓ | Fixed beta/theta. |
| `entrancearm` | ✓ | ✓ | Entrance arm length. |
| `exitarm` | ✓ | ✓ | Exit arm (kept for compatibility). |
| `SlitDist`, `Slit2Ph` | ✓ | ✓ | Slit distance and slit-to-pinhole distance, branch 0. |
| `cDriveEnergy`, `MZeroOrder`, `b2` | ✓ | ✓ | Mode-specific constants. |
| `gamma` | ✓ | ✓ | Gamma (configured). |
| `nm_per_step` | ✓ | ✓ | nm per step. |
| `Mslope`, `Moffset`, `Mquad`, `Mcubic` | ✓ | ✓ | Mirror angle calibration . |
| `Gslope`, `Goffset`, `Gquad`, `Gcubic` | ✓ | ✓ | Grating angle calibration . |
| `MtransMin`, `MtransMax`, `GtransMin`, `GtransMax` | ✓ | ✓ | Ranges of the angle transformation. |
| `polSmuX<n>`, `polSmuY<n>`, `polSmuZ<n>` | ✓ | ✓ | SMU positions for polarisation n. |
| `OffsetCorr<n>` | ✓ | ✓ | Offset correction for polarisation n. |

#### Exit slit, slit translation, pinhole, aperture, ASBL

| Name | Set | Get | Description |
| --- | :-: | :-: | --- |
| `ES_<n>_JOGSPEED` | ✓ | ✓ | Exit slit jog speed. |
| `ES_<n>_OFFSET`, `ES_<n>_EOFFSET` | ✓ | ✓ | Exit slit offset / encoder offset. |
| `ES_<n>_P1`, `ES_<n>_P2` | ✓ | ✓ | Exit slit motor parameters. |
| `ES_<n>_Slit2Ph` | ✓ | ✓ | Slit-to-pinhole distance of branch n. |
| `ES_<n>_SlitDist` | ✓ | ✓ | Slit distance of branch n. |
| `ES_<n>_ExitArm` | | ✓ | Exit arm of branch n. |
| `ES_<n>_MIN`, `ES_<n>_MAX` | | ✓ | Exit slit limits. |
| `TES_<n>_JOGSPEED` | ✓ | ✓ | Slit translation jog speed. |
| `TES_<n>_SetPos` | ✓ | | Load a position into the slit translation. |
| `TES_<n>_atZero`, `TES_<n>_Min`, `TES_<n>_Max` | | ✓ | Slit translation offset and limits. |
| `PH_<n>_OFFSET` | ✓ | ✓ | Pinhole offset. |
| `PH_<n>_EOFFSET` | | ✓ | Pinhole encoder offset. |
| `PH_<n>_SetPos` | ✓ | | Load a position into the pinhole. |
| `PH_<n>_TOLERANCE` | ✓ | ✓ | Pinhole tolerance. |
| `PH_<n>_MIN` | | ✓ | Pinhole minimum. |
| `PH_<n>_MAX` | | ✓ | Pinhole maximum. |
| `PH_<n>_JOGSPEED` | ✓ | ✓ | Pinhole jog speed. |
| `PH_<n>_<k>_offset` | | ✓ | Offset of fixed position k of pinhole n. |
| `AP_<n>_SIZE`, `AP_<n>_CENTER` | | ✓ | Aperture size and center. |
| `AP_<n>_SIZESTEP`, `AP_<n>_CENTERSTEP` | ✓ | ✓ | Aperture step widths. |
| `ASBL_<n>`, `ASBL_<n>_off` | ✓ | ✓ | ASBL blade n (1..4) and its offset. |

### 4.2 Integer parameters (`SPL` / `GPL`)

| Name | Set | Get | Description |
| --- | :-: | :-: | --- |
| `MonoMode` | | ✓ | Mono mode. |
| `formulaMode` | ✓ | ✓ | Mono formula mode. |
| `UserMode` | ✓ | ✓ | `0` standard (c = 2), `1` high resolution (c = 5), `2` high-order suppression (c = 1.4), `3` user defined. Not for SGMs. |
| `order` | ✓ | ✓ | Diffraction order (≠ 0). |
| `branch` | ✓ | ✓ | Calculation branch . |
| `DragonMode` | ✓ | ✓ | Dragon mode. |
| `UseCurrentAngle` | ✓ | ✓ | Use the current angle. |
| `WinkelTrans` | ✓ | ✓ | Angle transformation on/off. |
| `Naverage` | ✓ | ✓ | Number of averages (0..100). |
| `mirrorVelocity`, `gratingVelocity` | ✓ | ✓ | Drive velocities. |
| `HOLDPOSITION` | ✓ | ✓ | Hold position of grating and mirror. |
| `PositionTimer` | ✓ | ✓ | Position timer. |
| `PositionTime` | | ✓ | Position time of the grating. |
| `CheckBMT` | ✓ | ✓ | Check BMT. |
| `BL_READY` | ✓ | ✓ | Light on grating. |
| `SUSPEND` | | ✓ | Suspend flag. |
| `GK_STATUS` | | ✓ | Grating/mirror status. |
| `SW_STATUS` | | ✓ | Sweep status (bits 1 and 2 of `GST`). |
| `refStat` | | ✓ | Reference status. |
| `ErrorCount` | | ✓ | Error counter. |
| `gc_ACTIVE`, `gc_UNDEFINED` | | ✓ | Grating changer state. |
| `sweepState`, `sweepTics`, `sweepGapMeth` | ✓ | ✓ | Sweep state, tics, gap method. |
| `ZeitFaktor` | ✓ | ✓ | Sweep time factor. |
| `HeyRate`, `HeySelection` | ✓ | ✓ | Heydemann rate/selection. |
| `HeyTeiler` | | ✓ | Heydemann divider. |
| `ik_SELECT`, `ikHeydemann`, `ikACTIVE` | ✓ | ✓ | Interpolation selection, Heydemann on/off, active. |
| `ik_CONFIG` | | ✓ | Interpolation configuration. |
| `monoIdMode`, `IdOn` | ✓ | ✓ | Mono–ID coupling on/off. |
| `IdDecouple` | ✓ | | ID decouple mode. |
| `ofEnableShift` | ✓ | | Onfly scans also move the ID shift. |
| `IdCom`, `IdRemLocal`, `UndulatorFREI` | ✓ | ✓ | ID communication, remote/local, ID free. |
| `UN_STATUS` | | ✓ | ID status. |
| `idHarmonic` | ✓ | ✓ | Current ID harmonic. |
| `IdHarmonic<n>` | ✓ | ✓ | Harmonic of ID table n (odd harmonics). |
| `PolActive`, `OffsetActive` | | ✓ | Active polarisation. |
| `POL_STORE` | ✓ | ✓ | Polarisation store flag. |
| `NPols` | | ✓ | Number of polarisations. |
| `SMUstatus` | | ✓ | SMU busy/done. |
| `SMU_<n>_PosA`, `_PosB`, `_PosC` | | ✓ | SMU positions (n = 0..2). |
| `TES_0_FOLLOW` | ✓ | ✓ | Exit slit translation follows the energy. |
| `ES_STATUS` | | ✓ | Exit slit status, current branch. |
| `ES_REF_STAT`, `ES_PLATEAU` | | ✓ | Exit slit reference status and plateau, branch 0. |
| `PLATEAU` | | ✓ | Plateau, current branch. |
| `ES_<n>_STATUS`, `ES_<n>_REF_STAT`, `ES_<n>_PLATEAU` | | ✓ | Same, per branch. |
| `PH_<n>_CalOffsetNr` | ✓ | ✓ | Calibration pinhole number (≥ 1). |
| `PH_<n>_EVENT` | ✓ | ✓ | Start/stop event number. |
| `PH_<n>_MotNr` | | ✓ | Motor number. |
| `PH_<n>_LLS`, `PH_<n>_HLS` | | ✓ | Low/high limit switch. |
| `AmcMode` | ✓ | ✓ | AMC mode. |
| `AmcIdMode` | ✓ | ✓ | AMC/ID mode. |
| `AmcBranch`, `AmcTcpRule` | ✓ | ✓ | AMC branch, TCP rule. |
| `AmcTcpPort` | | ✓ | AMC TCP port. |
| `BOARD_<i>_AKTION` | ✓ | | Run a board action. |
| `BOARD_<i>_STATUS` | | ✓ | Board status. |
| `asbl_debug` | ✓ | ✓ | ASBL debug level. |

### 4.3 String parameters (`SPS`)

String parameters can only be set; reading them is not supported yet.

| Name | Description |
| --- | --- |
| `MonoTableName` | Table file to use. |
| `idFileName` | ID table file (loaded when set). |
| `IdFileName<n>` | ID table file n (0 = main table); loaded when set. |
| `ClientName<n>` | AMC client name (n = 0..4). |
| `AllowClient<n>` | AMC allowed client (n = 0..4). |
| `MonError` | Log the value as an error message. |

---

## 5. Not yet supported (work in progress)

These commands from the old mono application are planned but not available yet:

| Command | Description |
| --- | --- |
| `:` | Energy with a packed reply (shorter, for serial lines). |
| `GPS` | Read a string parameter. |
| `UGS`, `USS` | Read and set the undulator shift. |
| `UGF`, `USF`, `UGST` | Further undulator commands (to be documented). |
