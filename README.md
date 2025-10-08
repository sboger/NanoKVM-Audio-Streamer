# Audio Streamer for LicheeRV Nano boards with a mems microphone



I created this in a few hours as a test of gemini-cli. I coded no part of this. I only altered this paragraph by hand. Everything else was created by gemini-cli. My coding system is a mac air, the compile system is a LicheeRV Nano (https://www.amazon.com/dp/B0DGL82524) running Debian 13 Trixie (https://github.com/scpcom/sophgo-sg200x-debian). This is a minimal C application (11k) that captures audio from the mems microphone using ALSA and streams it over HTTP. The audio is streamed as raw, lossless LPCM data.

## Dependencies

You build this on the LicheeRV Nano. You need to have build-essentials and the ALSA library development package installed. On Sophgo sg200x Debian you can install it with:

```bash
sudo apt-get update
sudo apt-get install build-essential libasound2-dev
```

## Building

To build the application, run:

```bash
make clean  # to erase the binary that comes with the repo
make
```

This will create an executable file named `audio_streamer`.


## Running

To run the server, execute the compiled binary:

```bash
./audio_streamer
```

To run the server as a daemon (in the background), use the `-d` flag:

```bash
./audio_streamer -d
```

The server will start listening on port 8080. It will print a message to the console indicating that it's running.

## Listening to the stream

You can listen to the audio stream using a media player that supports raw LPCM streams over HTTP, such as VLC.

To do so in VLC:
1. Open VLC.
2. Go to "Media" -> "Open Network Stream...".
3. Enter the URL of the stream: `http://<server_ip>:8080`, where `<server_ip>` is the IP address of the machine running the `audio_streamer`.
4. Click "Play".

You should now be able to hear the audio from the microphone.

Note: On OSX in Chrome, I can also load the ip directly in the browser and get a player.

## How it works

The server listens for incoming TCP connections on port 8080. When a client connects, it forks a new process to handle the client. This child process opens the default ALSA capture device (`hw:0,0`), sets the audio parameters (48kHz, 16-bit Little Endian, mono), and starts reading audio data.

The audio data is sent to the client with an HTTP header that specifies the content type as `audio/wav`. A 44-byte WAV header is sent at the beginning of the stream, which allows clients like VLC to correctly interpret the audio format.

## Stopping the server

If you are running the server in the foreground, you can stop it by pressing `Ctrl+C`.

If you are running the server as a daemon, you can find its process ID (PID) and kill it:

```bash
ps aux | grep audio_streamer
kill <PID>
```
