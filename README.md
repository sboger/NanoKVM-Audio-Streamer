# NanoKVM Audio Streamer

![License: GPL-3.0](https://img.shields.io/badge/license-GPL--3.0-blue)
![Package: Debian 13](https://img.shields.io/badge/package-Debian%2013%20trixie-A80030?logo=debian&logoColor=white)
![Arch](https://img.shields.io/badge/arch-riscv64%20%7C%20arm64%20%7C%20amd64-informational)

Stream the onboard MEMS microphone of a **Sipeed NanoKVM** (LicheeRV Nano /
Sophgo SG2002) to any browser or media player as lossless WAV over HTTP.
Roughly 11 KB of C: one ALSA capture, a `select()` loop, no dependencies
beyond `libasound2`.

Built and tested on **Debian 13 (trixie), riscv64**, from the
[scpcom/sophgo-sg200x-debian](https://github.com/scpcom/sophgo-sg200x-debian)
image.

---

## Example recording

[![Waveform of the example recording](examples/nanokvm-audio1-waveform.png)](examples/nanokvm-audio1.wav)

**[nanokvm-audio1.wav](examples/nanokvm-audio1.wav)** — click the waveform to
play or download.

Recorded in a small server cabinet, directly from the MEMS microphone of a
**NanoKVM Lite** running the latest
[scpcom/sophgo-sg200x-debian](https://github.com/scpcom/sophgo-sg200x-debian)
firmware. 15 seconds, 48 kHz, mono, 16-bit little-endian — the raw stream as
served on port 8080, with no processing applied.

> The microphone sits on the KVM board itself. It hears the room around the
> KVM, **not** the HDMI audio of the machine being controlled.

---

## Install (Debian package)

A prebuilt riscv64 package is committed under
[`dist/`](dist/nanokvm-audio-streamer_1.0.0_riscv64.deb) — download it
straight from GitHub:

```bash
curl -LO https://github.com/sboger/LicheeRV-Audio-Streamer/raw/main/dist/nanokvm-audio-streamer_1.0.0_riscv64.deb
sha256sum nanokvm-audio-streamer_1.0.0_riscv64.deb
```

`sha256: ba8fb897d7098da731da95e20a1973561c5ee61198542d85b44be7c4792455d9`

Then install on the NanoKVM:

```bash
sudo apt install ./nanokvm-audio-streamer_1.0.0_riscv64.deb
```

The package installs `/usr/bin/audio_streamer`, enables and starts
`nanokvm-audio-streamer.service`, and ships its configuration in
`/etc/default/nanokvm-audio-streamer`.

Verify:

```bash
systemctl status nanokvm-audio-streamer
curl http://localhost:8080/healthz
```

Uninstall with `sudo apt remove nanokvm-audio-streamer` (the configuration
file in `/etc/default` is left in place; purge removes it).

---

## Listening

| Client | How |
| --- | --- |
| Browser | Open `http://<kvm-ip>:8080/` — Chrome, Safari and Firefox render a native player |
| VLC | *Media → Open Network Stream…* → `http://<kvm-ip>:8080/` |
| ffplay | `ffplay -nodisp -autoexit http://<kvm-ip>:8080/` |
| ffmpeg | `ffmpeg -i http://<kvm-ip>:8080/ -t 10 capture.wav` |
| curl | `curl --max-time 10 http://<kvm-ip>:8080/ -o capture.wav` |

Up to 16 clients can listen at the same time; each one receives the same
live PCM.

## HTTP endpoints

| Path | Response |
| --- | --- |
| `/` (any path except `/healthz`) | `audio/wav` — LPCM stream, 48 kHz mono by default |
| `/healthz`, `/health` | `application/json` status |
| `HEAD /` | stream headers only, no body |

```json
{"status":"ok","device":"hw:0,0","rate":48000,"channels":1,"clients":1,"served":3,"uptime_s":412}
```

`/healthz` is cheap to poll, so it drops straight into Checkmk, Zabbix, Home
Assistant REST sensors or a cron watchdog.

---

## Configuration

Edit `/etc/default/nanokvm-audio-streamer`:

```bash
OPTIONS="--port 8080 --device hw:0,0 --rate 48000 --verbose"
```

then restart the service:

```bash
sudo systemctl restart nanokvm-audio-streamer
```

### Options

| Flag | Default | Meaning |
| --- | --- | --- |
| `-p`, `--port N` | `8080` | TCP port to listen on |
| `-a`, `--address A` | `0.0.0.0` | bind address |
| `-D`, `--device D` | `hw:0,0` | ALSA capture device (`arecord -l` lists them) |
| `-r`, `--rate N` | `48000` | sample rate in Hz (the device picks the nearest supported) |
| `-c`, `--channels N` | `1` | channel count |
| `-b`, `--period N` | `20` | ALSA period in milliseconds (lower = less latency) |
| `-d`, `--daemon` | off | detach and run in the background (not needed under systemd) |
| `-v`, `--verbose` | off | log connections, xruns and errors to stderr |
| `-V`, `--version` | | print version |
| `-h`, `--help` | | usage |

On the NanoKVM the capture device is card 0, `cv182xa_adc`, exposed as
`hw:0,0` at 48 kHz mono.

---

## Build from source

Dependencies:

```bash
sudo apt-get update
sudo apt-get install build-essential libasound2-dev
```

Build and run:

```bash
make
./audio_streamer -v            # foreground, logs to the terminal
./audio_streamer -d            # detached
./audio_streamer -p 9090 -v    # custom port
```

`make install` puts the binary in `/usr/local/bin` (honours `PREFIX` and
`DESTDIR`).

## Build the Debian package

On the target device (the package is `Architecture: any` and built natively):

```bash
sudo apt-get install build-essential libasound2-dev debhelper
dpkg-buildpackage -us -uc -b
```

The result lands one directory up:

```
../nanokvm-audio-streamer_1.0.0_riscv64.deb
```

Install it with `sudo apt install ../nanokvm-audio-streamer_1.0.0_riscv64.deb`.
The source format is `3.0 (native)`, so no separate orig tarball is needed.

### What the package contains

```
/usr/bin/audio_streamer
/lib/systemd/system/nanokvm-audio-streamer.service
/etc/default/nanokvm-audio-streamer
```

The unit runs the streamer as a `DynamicUser` with `SupplementaryGroups=audio`
so it can open `/dev/snd` without running as root. It also applies
`NoNewPrivileges`, `ProtectSystem=strict`, `ProtectHome`, `PrivateTmp`,
`RestrictAddressFamilies`, `LockPersonality` and
`MemoryDenyWriteExecute`. Restart policy is `on-failure`, `RestartSec=2s`.

---

## How it works

1. The parent opens the ALSA capture device and negotiates the hardware
   parameters, so the WAV header always reports the real rate.
2. It forks: the child reads PCM frames and writes them into a pipe; the
   parent serves HTTP.
3. Each accepted connection gets an HTTP 200 with `Content-Type: audio/wav`
   followed by a 44-byte WAV header. The `RIFF` and `data` sizes are set to
   `0xFFFFFFFF`, the conventional "unknown / streaming" value, so players
   keep reading until the connection closes.
4. The parent `select()`s on the listening socket, the pipe and the client
   sockets, broadcasting each PCM chunk to every connected client. Slow or
   dead clients are dropped (2 s send timeout, `SIGPIPE` ignored) without
   disturbing the others.
5. `SIGTERM`/`SIGINT` set a flag, the loop exits and the capture child is
   reaped — which is exactly what `systemctl stop` needs.
6. Capture overruns (`-EPIPE`) and suspend/resume (`-ESTRPIPE`) are recovered
   in place instead of killing the process.

---

## Troubleshooting

**No sound / all silence.** The MEMS mic is low-gain. Test it directly:

```bash
arecord -D hw:0,0 -f S16_LE -r 48000 -c 1 -d 2 /tmp/test.wav
ffmpeg -i /tmp/test.wav -af volumedetect -f null -
```

Real signal shows a peak in the thousands (e.g. −20 dBFS). A peak near 0
means the mic is muted or the wrong device is selected.

**Port already in use.**

```bash
ss -tlnp | grep 8080
```

**Service won't start.** Check the log and the device:

```bash
journalctl -u nanokvm-audio-streamer -n 50 --no-pager
arecord -l
```

`ConditionPathExists=/dev/snd` keeps the unit from starting on hosts without
a sound card, so a silent "condition failed" is expected there.

**`pkill -f audio_streamer` over SSH kills your own shell** — the remote
command line contains the string. Use `pkill -x audio_streamer` or the
service.

---

## Security

There is **no authentication**: anyone who can reach port 8080 can listen to
the microphone. Keep it on a trusted LAN, or front it with an authenticating
reverse proxy. Do not publish it to the internet.

---

## License

GPL-3.0-or-later — see [LICENSE](LICENSE).

The first version of this project was scaffolded in a few hours as a test of
`gemini-cli`; the code, packaging and documentation here have since been
reworked.
