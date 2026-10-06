#!/usr/bin/env python3
"""Record STE/Falcon DMA sound through the Hatari agent API.

Reads the playing DMA buffer's registers, then repeatedly copies the
part of the (repeating) buffer that playback has just passed and writes
it to a WAV file.  Works with --sound off too: the DMA sound hardware is
still emulated.  8 bit mono and stereo only.

    dma_sound_capture.py out.wav [seconds] [--rate HZ]

The sample rate comes from the Falcon crossbar prescaler ($ff8935, 25.175
MHz clock) or the STE rate bits ($ff8921); --rate overrides it.
"""
import json
import os
import sys
import time
import urllib.request
import wave

API = os.environ.get("HATARI_API", "http://127.0.0.1:7777")


def mem(addr, n):
    url = "%s/mem?addr=%d&len=%d" % (API, addr, n)
    with urllib.request.urlopen(url, timeout=10) as r:
        return bytes.fromhex(json.load(r)["hex"])


def dma_regs():
    r = mem(0xFF8900, 0x40)
    ctl = r[0x01]
    start = (r[0x03] << 16) | (r[0x05] << 8) | r[0x07]
    cur = (r[0x09] << 16) | (r[0x0B] << 8) | r[0x0D]
    end = (r[0x0F] << 16) | (r[0x11] << 8) | r[0x13]
    mode = r[0x21]
    prescale = r[0x35] & 0x0F
    return ctl, start, cur, end, mode, prescale


def main():
    args = [a for a in sys.argv[1:]]
    rate = None
    if "--rate" in args:
        i = args.index("--rate")
        rate = int(args[i + 1])
        del args[i:i + 2]
    if not args:
        sys.exit(__doc__)
    out_name = args[0]
    secs = float(args[1]) if len(args) > 1 else 5.0

    ctl, start, cur, end, mode, prescale = dma_regs()
    size = end - start
    if not (ctl & 1) or size <= 0:
        sys.exit("DMA sound is not playing (control $%02x)" % ctl)
    kind = (mode >> 6) & 3
    if kind == 1:
        sys.exit("16 bit DMA sound is not supported")
    channels = 1 if kind == 2 else 2
    if rate is None:
        rate = (25175000 // 256 // (prescale + 1) if prescale
                else (6258, 12517, 25033, 50066)[mode & 3])

    last = (cur - start) & ~(channels - 1)
    data = bytearray()
    t0 = time.time()
    while time.time() - t0 < secs:
        time.sleep(0.06)
        cur = dma_regs()[2]
        pos = (cur - start) & ~(channels - 1)
        n = (pos - last) % size
        if not n:
            continue
        buf = mem(start, size)
        chunk = (buf[last:last + n] if last + n <= size
                 else buf[last:] + buf[:last + n - size])
        data += bytes((b + 128) & 255 for b in chunk)    # signed -> unsigned
        last = pos

    with wave.open(out_name, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(1)
        w.setframerate(rate)
        w.writeframes(bytes(data))
    frames = len(data) // channels
    print("%s: %d frames, %s at %d Hz, %.2f s of audio in %.2f s"
          % (out_name, frames, "stereo" if channels == 2 else "mono",
             rate, frames / rate, secs))


if __name__ == "__main__":
    main()
