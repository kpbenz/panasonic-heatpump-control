#!/usr/bin/env python3
"""
Panasonic AC IR Protocol Decoder
================================

Decodes the 216-bit, two-frame IR protocol emitted by the remote control
of a Panasonic CS-Z25UFRAW air conditioner, using the raw pulse output
of ``ir-ctl`` (from the v4l-utils package) on a Raspberry Pi.

The decoder reads a continuous stream of mark/space durations from
``ir-ctl --receive``, filters out glitches, detects the frame header,
and classifies each mark/space pair into a '0' or '1' bit using
pulse-distance encoding:

    * bit '0':  ~420 us mark followed by a ~440 us space
    * bit '1':  ~420 us mark followed by a ~1300 us space
    * header:   ~3500 us mark followed by a ~1750 us space
    * frame gap: ~10 ms space separating the two frames

The two frames are collected separately and printed either as
hexadecimal values (default) or as raw binary strings (``--binary``).

Hardware setup (IR receiver on a Raspberry Pi 3)
------------------------------------------------
Common IR receiver modules (TSOP382, TSOP4838, VS1838B):

    VCC: 5V or 3.3V (check your module specs)
    GND: Ground
    OUT/Signal: GPIO pin (e.g., GPIO 17, 22, 27)

Wiring diagram (for a 3-pin IR receiver)::

    IR Receiver Module
    |- VCC (5V)    -> Raspberry Pi 5V pin (pin 2 or 4)
    |- GND         -> Raspberry Pi GND (pin 6, 9, 14, 20, 25, 30, 34, 39)
    '- OUT         -> GPIO pin (e.g., GPIO 17 on pin 11)

Enable the kernel IR receiver in /boot/config.txt::

    dtoverlay=gpio-ir,gpio_pin=17

Usage
-----
    ./lirc-decoder.py [-v|--verbose] [-b|--binary]

    -v, --verbose   Print per-pulse decoding details to stdout
    -b, --binary    Print the decoded frames as binary strings
                    instead of hexadecimal

Press Ctrl+C to exit.

Copyright (c) 2026 karsten@benz-engineering.co.nz

Licensed under the MIT License:

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
"""

import os
import subprocess
import sys
import argparse
from collections import deque

# Global verbosity flag; set from the command line in __main__.
verbose = False


def get_ir_ctl_stream():
    """Launch ``ir-ctl`` and yield one line of raw pulse data per IR burst.

    Runs ``ir-ctl --receive=/dev/stdout`` as a subprocess and reads its
    output line by line. ir-ctl terminates each received burst with a
    trailing ``# timeout NNNNN`` marker; everything up to (but not
    including) the ``#`` is yielded to the caller as a single string of
    space-separated signed pulse durations, e.g.::

        "+3473 -1734 +420 -442 ... +422"

    where positive values are marks (IR carrier on) and negative values
    are spaces (carrier off), both in microseconds.

    Yields:
        str: One burst of pulse data, with the timeout marker stripped.

    Raises:
        SystemExit: If the subprocess fails for any reason other than
            a KeyboardInterrupt.
    """
    cmd = ['/usr/bin/ir-ctl', '--receive=/dev/stdout']
    try:
        with subprocess.Popen(cmd,
                            stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE,
                            universal_newlines=True) as proc:

            print("Waiting for IR data (point remote at receiver)...", file=sys.stderr)

            while True:
                line = proc.stdout.readline().strip()
                if not line:
                    # An empty line either means "no data yet" or the
                    # subprocess has exited; poll() distinguishes the two.
                    if proc.poll() is not None:
                        break
                    continue

                # ir-ctl appends "# timeout NNNNN" at the end of a burst.
                # Only lines containing this marker represent a complete
                # burst; yield the pulse data portion before the '#'.
                index = line.find('#')
                if index != -1:
                    yield line[:index]

    except KeyboardInterrupt:
        print("\nStopping IR receiver...", file=sys.stderr)
        proc.terminate()
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


def decode_manchester(ir_stream):
    """Decode pulse-distance encoded IR bursts into two bit frames.

    Consumes complete bursts from ``ir_stream`` (as produced by
    :func:`get_ir_ctl_stream`) and decodes each one into the two frames
    of the Panasonic AC protocol. For every burst the decoder:

    1. Splits the line into individual pulse durations and drops
       glitches shorter than ``GLITCH_THRESHOLD``.
    2. Walks the pulses in mark/space pairs, recognising in order:
       the header (long mark + long space, which arms decoding),
       the inter-frame pause (~10 ms space, which switches from
       frame 0 to frame 1), and finally data bits.
    3. Classifies each data pair by its space duration:
       space > BIT_PERIOD -> '1', otherwise '0'.
    4. Prints both frames, either as hexadecimal values (default) or
       as binary bit strings when ``--binary`` was given.

    Timing constants (all in microseconds):
        BIT_PERIOD:       Nominal duration of one full bit cell.
                          A space longer than this marks a '1'.
        TIMEOUT_PERIOD:   Spaces longer than this (10 bit periods)
                          are treated as the inter-frame gap.
        TOLERANCE:        Fractional timing slack (30%) applied when
                          matching header and pause conditions.
        GLITCH_THRESHOLD: Pulses shorter than a quarter bit period are
                          discarded as receiver noise.

    Args:
        ir_stream: Iterable of strings, each containing one burst of
            space-separated signed pulse durations.

    Note:
        Despite the function name, the Panasonic AC protocol is
        pulse-distance encoded, not Manchester encoded: the information
        is carried by the length of the space following each mark.
    """
    # Retained for potential lookahead/smoothing of pulse pairs;
    # currently unused by the decoding loop below.
    pulse_queue = deque(maxlen=2)

    # --- Protocol timing parameters -----------------------------------
    BIT_PERIOD = 840                        # Nominal bit cell length (us)
    TIMEOUT_PERIOD = 10 * BIT_PERIOD        # Inter-frame gap threshold (us)
    TOLERANCE = 0.3                         # Allowed timing variation (30%)
    GLITCH_THRESHOLD = int(BIT_PERIOD / 4)  # Ignore pulses shorter than this

    try:
        for line in ir_stream:
            if verbose:
                print(f"Line: {line}\n\n")

            # bits[0] holds frame 0, bits[1] holds frame 1.
            bits = [[], []]
            frame = 0            # Index of the frame currently being filled
            i = 0                # Position within the pulse list
            half_bit = BIT_PERIOD / 2
            tolerance_window = half_bit * TOLERANCE  # Currently unused

            # ---- Step 1: parse and de-glitch the pulse list ----------
            pulses = []
            for p in line.split():
                try:
                    if abs(int(p)) > GLITCH_THRESHOLD:
                        pulses.append(int(p))
                    elif verbose:
                        print(f"{p} dropped as glitch")
                except ValueError:
                    # Non-numeric token in the ir-ctl output; skip it.
                    print("ValueError.")
                    continue

            if verbose:
                print(f"Pulses: {pulses}")

            # Decoding is only armed after the header has been seen,
            # so leading noise before the header is ignored.
            decoding = False

            # ---- Step 2: walk the pulses in mark/space pairs ---------
            while i < len(pulses) - 1:
                p1 = pulses[i]      # Mark (positive duration)
                p2 = pulses[i + 1]  # Space (negative duration)
                if verbose:
                    print(f"{i:3d}  {p1:5d} {p2:5d}", end="")

                # Frame boundary: a normal-length mark followed by a
                # very long space (~10 ms) separates frame 0 from
                # frame 1. Switch to filling the second frame.
                if abs(p1) < 1.5 * (BIT_PERIOD + TOLERANCE) and abs(p2) > TIMEOUT_PERIOD:
                    if verbose:
                        print(" -> New Frame.")
                    frame = 1
                    i += 2
                    continue

                # Header: both mark and space well above one bit period
                # (~3500 us / ~1750 us). The first header arms decoding;
                # a second occurrence (start of frame 1) is just noted.
                if abs(p1) > 1.5 * (BIT_PERIOD + TOLERANCE) and abs(p2) > 1.5 * (BIT_PERIOD + TOLERANCE):
                    if not decoding:
                        if verbose:
                            print(" -> Start decoding.")
                        decoding = True
                    else:
                        if verbose:
                            print(" -> Header.")
                    i += 2
                    continue

                # Anything before the first header is skipped one pulse
                # at a time until the header pattern aligns.
                if not decoding:
                    i += 1
                    continue

                # Long trailing pause (end of transmission).
                if abs(p1) < BIT_PERIOD and abs(p2) > 10 * (BIT_PERIOD + TOLERANCE):
                    if verbose:
                        print(" -> Pause.")
                    i += 2
                    continue

                # Data bit: the mark is always short (~420 us); the
                # length of the following space determines the value.
                #   space > BIT_PERIOD  -> '1'  (~1300 us space)
                #   space <= BIT_PERIOD -> '0'  (~440 us space)
                if abs(p1) < BIT_PERIOD:
                    if abs(p2) > BIT_PERIOD:
                        bits[frame].append('1')
                        if verbose:
                            print(f" -> add 1 {len(bits[frame])}")
                    else:
                        bits[frame].append('0')
                        if verbose:
                            print(f" -> add 0 {len(bits[frame])}")
                    i += 2
                    continue

            if verbose:
                print(f"bits:{bits}")

            # ---- Step 3: format and print the decoded frames ---------
            bit_string0 = ''.join(bits[0])
            bit_string1 = ''.join(bits[1])
            try:
                hex_string0 = hex(int(bit_string0, 2))
                hex_string1 = hex(int(bit_string1, 2))
            except ValueError:
                # An empty or otherwise invalid bit string cannot be
                # converted; show empty quotes instead of crashing.
                hex_string0 = "''"
                hex_string1 = "''"
            if verbose:
                print(f"Frame 0: length {len(bits[0])}: {bit_string0} {hex_string0}", end='\n', file=sys.stdout)
                print(f"Frame 1: length {len(bits[1])}: {bit_string1} {hex_string1}", end='\n', file=sys.stdout)
            if args.binary:
                print(f"{bit_string0} {bit_string1}", end='\n', file=sys.stdout)
            else:
                print(f"{hex_string0} {hex_string1}", end='\n', file=sys.stdout)

    except KeyboardInterrupt:
        # Show whatever was decoded so far when interrupted mid-burst.
        if bits:
            print(f"\nPartial frame: {''.join(bits)}", file=sys.stderr)


if __name__ == "__main__":

    # ---- Command line interface --------------------------------------
    parser = argparse.ArgumentParser(
        description='Decode Panasonic A/C IR signals'
    )

    parser.add_argument('-v', '--verbose', action='store_true',
                        help='Enable verbose output')
    parser.add_argument('-b', '--binary', action='store_true',
                        help='Output in binary format')

    # Note: 'args' is intentionally module-level so decode_manchester()
    # can read args.binary directly.
    args = parser.parse_args()
    verbose = args.verbose
    print("Panasonic A/C IR Decoder (using ir-ctl)", file=sys.stderr)
    print("Press Ctrl+C to exit\n", file=sys.stderr)

    # ir-ctl is part of the v4l-utils package; bail out early with a
    # helpful message if it is not installed.
    if not os.path.exists('/usr/bin/ir-ctl'):
        print("Error: ir-ctl not found. Install with: sudo apt install ir-ctl", file=sys.stderr)
        sys.exit(1)

    # Wire the ir-ctl pulse stream into the decoder and run until Ctrl+C.
    decode_manchester(get_ir_ctl_stream())

# vi:sw=4 ts=6:
