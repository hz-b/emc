# The `:`- binary energy command

The `:` command is part of the old VxWorks EMC server (`vxEMC2.c`), which
clients still use over a serial line at 9600 baud. The new EMC server
does not support `:` yet.

## Purpose

`:` returns the current mono energy as 4 raw bytes instead of text. At 9600 baud a character takes about 1 ms, so the 4-byte reply arrives in about **4 ms**. The text reply of `GPE` for 400 eV, `t 400.000000` plus CR, is 13 characters,
about **13.5 ms**.

## Request

```
:<CR>
```

No argument. Bytes: `3A 0D`.

## Reply

**Success:** exactly 4 bytes, the energy in eV as a 32-bit IEEE 754 float
(`float`). There is no `t` prefix and **no CR** after the 4 bytes.

The bytes are sent in the memory order of the IOC CPU (the server copies the
float through a byte array). The old server runs on VxWorks 5.4 on Motorola
MVME 162 boards (68040 CPU), which are big-endian, so the most significant byte
comes first.

| Energy (eV) | Bytes sent (big-endian) |
| ---: | --- |
| 400.0 | `43 C8 00 00` |
| 564.0 | `44 0D 00 00` |
| 1000.0 | `44 7A 00 00` |

**Failure:** the text reply `f` followed by CR, 2 bytes: `66 0D`. This happens
when the mono does not answer the energy request, for example when it is not
initialised. Use `GLE` to read the error text.

The value is the same energy that `GPE` returns, rounded to float precision
(about 7 significant digits, so roughly ±0.00003 eV at 500 eV).

## Reading the reply in a client

1. Send `:` followed by CR.
2. Read exactly 4 bytes. Do not read up to a CR.
3. Convert the 4 bytes from big-endian to a float.

A failure reply is only 2 bytes (`f` and CR). A client that waits for 4 bytes
needs a read timeout to detect it.

## Known problem: 564 to 568 eV

For energies from 564 eV up to (not including) 568 eV, the second byte of the float is `0D`, which is the CR character:

| Energy (eV) | Bytes |
| ---: | --- |
| 563.97 | `44 0C FE 14` |
| 564.0 | `44 0D 00 00` |
| 567.99 | `44 0D FF 5C` |
| 568.0 | `44 0E 00 00` |

A client that reads up to CR instead of reading 4 bytes stops after the second
byte in this range. The same can happen for single values elsewhere whenever
the third or fourth byte is `0D`. Reading a fixed 4 bytes avoids the problem.

## Notes for the new EMC server implementation

In order to keep existing clients keep working:

- Send the 4 float bytes in big-endian order explicitly. The new IOC may run on
  a little-endian CPU, and copying the float's memory as before would reverse
  the bytes.
- Send no `t` and no terminator after the 4 bytes, regardless of the
  configured reply terminator.