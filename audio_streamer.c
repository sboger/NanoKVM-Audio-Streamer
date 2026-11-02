/*
 * audio_streamer - stream a NanoKVM / LicheeRV Nano MEMS microphone over HTTP
 *
 * Captures mono S16_LE PCM from an ALSA capture device and serves it to any
 * number of HTTP clients as a WAV (LPCM) stream.
 *
 * Endpoints:
 *   /          - the audio stream (audio/wav, LPCM, chunked-unknown length)
 *   /healthz   - JSON status
 *
 * License: GPL-3.0-or-later. See LICENSE.
 */

#define _GNU_SOURCE

#include <alsa/asoundlib.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define PROGRAM "audio_streamer"
#define VERSION "1.0.0"

#define DEFAULT_PORT 8080
#define DEFAULT_ADDRESS "0.0.0.0"
#define DEFAULT_DEVICE "hw:0,0"
#define DEFAULT_RATE 48000
#define DEFAULT_CHANNELS 1
#define DEFAULT_PERIOD_MS 20

#define MAX_CLIENTS 16
#define READ_BUFFER 8192
#define REQUEST_BUFFER 1024
#define REQUEST_TIMEOUT_MS 700
#define SEND_TIMEOUT_S 2

static volatile sig_atomic_t g_running = 1;
static int g_verbose = 0;
static unsigned int g_rate = DEFAULT_RATE;
static unsigned int g_channels = DEFAULT_CHANNELS;
static const char *g_device = DEFAULT_DEVICE;

static void log_msg(const char *fmt, ...)
{
    va_list ap;
    char stamp[32];
    time_t now = time(NULL);
    struct tm tm;

    if (!g_verbose)
        return;

    localtime_r(&now, &tm);
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);

    fprintf(stderr, "%s %s: ", stamp, PROGRAM);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static void on_signal(int sig)
{
    (void)sig;
    g_running = 0;
}

static void daemonize(void)
{
    pid_t pid;
    int fd;

    pid = fork();
    if (pid < 0)
        exit(EXIT_FAILURE);
    if (pid > 0)
        exit(EXIT_SUCCESS);

    if (setsid() < 0)
        exit(EXIT_FAILURE);

    pid = fork();
    if (pid < 0)
        exit(EXIT_FAILURE);
    if (pid > 0)
        exit(EXIT_SUCCESS);

    umask(0);
    if (chdir("/") != 0)
        exit(EXIT_FAILURE);

    fd = open("/dev/null", O_RDWR);
    if (fd >= 0) {
        dup2(fd, STDIN_FILENO);
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        if (fd > STDERR_FILENO)
            close(fd);
    }
}

/*
 * Open the capture device and negotiate parameters. The negotiated rate is
 * returned through *rate so the WAV header always describes reality.
 */
static snd_pcm_t *capture_open(unsigned int *rate, unsigned int period_ms)
{
    snd_pcm_t *handle = NULL;
    snd_pcm_hw_params_t *params = NULL;
    snd_pcm_uframes_t period_size;
    unsigned int requested = *rate;
    int dir = 0;
    int err;

    err = snd_pcm_open(&handle, g_device, SND_PCM_STREAM_CAPTURE, 0);
    if (err < 0) {
        fprintf(stderr, "%s: cannot open capture device %s: %s\n",
                PROGRAM, g_device, snd_strerror(err));
        return NULL;
    }

    snd_pcm_hw_params_alloca(&params);
    if ((err = snd_pcm_hw_params_any(handle, params)) < 0)
        goto fail;

    if ((err = snd_pcm_hw_params_set_access(handle, params,
                                            SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
        goto fail;

    if ((err = snd_pcm_hw_params_set_format(handle, params,
                                            SND_PCM_FORMAT_S16_LE)) < 0)
        goto fail;

    if ((err = snd_pcm_hw_params_set_channels(handle, params,
                                              (unsigned int)g_channels)) < 0)
        goto fail;

    if ((err = snd_pcm_hw_params_set_rate_near(handle, params, &requested, &dir)) < 0)
        goto fail;

    period_size = (snd_pcm_uframes_t)((uint64_t)requested * period_ms / 1000);
    if (period_size < 64)
        period_size = 64;
    dir = 0;
    snd_pcm_hw_params_set_period_size_near(handle, params, &period_size, &dir);

    if ((err = snd_pcm_hw_params(handle, params)) < 0)
        goto fail;

    snd_pcm_hw_params_get_rate(params, &requested, &dir);
    *rate = requested;

    if ((err = snd_pcm_prepare(handle)) < 0)
        goto fail;

    snd_pcm_start(handle);
    log_msg("capture open: device=%s rate=%u channels=%u period=%lu frames",
            g_device, *rate, (unsigned int)g_channels,
            (unsigned long)period_size);
    return handle;

fail:
    fprintf(stderr, "%s: cannot configure capture device %s: %s\n",
            PROGRAM, g_device, snd_strerror(err));
    snd_pcm_close(handle);
    return NULL;
}

/* Write exactly len bytes, coping with partial writes. Returns 0 or -1. */
static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t left = len;

    while (left > 0) {
        ssize_t n = write(fd, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        p += n;
        left -= (size_t)n;
    }
    return 0;
}

static void write_wav_header(int fd, unsigned int rate, unsigned int channels)
{
    unsigned char header[44];
    uint32_t byte_rate = rate * channels * 2;
    uint16_t block_align = (uint16_t)(channels * 2);

    memcpy(header, "RIFF", 4);
    header[4] = 0xFF; header[5] = 0xFF; header[6] = 0xFF; header[7] = 0xFF;
    memcpy(header + 8, "WAVE", 4);

    memcpy(header + 12, "fmt ", 4);
    header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;
    header[20] = 1; header[21] = 0;                 /* PCM */
    header[22] = (unsigned char)(channels & 0xFF);
    header[23] = (unsigned char)(channels >> 8);
    header[24] = (unsigned char)(rate & 0xFF);
    header[25] = (unsigned char)((rate >> 8) & 0xFF);
    header[26] = (unsigned char)((rate >> 16) & 0xFF);
    header[27] = (unsigned char)((rate >> 24) & 0xFF);
    header[28] = (unsigned char)(byte_rate & 0xFF);
    header[29] = (unsigned char)((byte_rate >> 8) & 0xFF);
    header[30] = (unsigned char)((byte_rate >> 16) & 0xFF);
    header[31] = (unsigned char)((byte_rate >> 24) & 0xFF);
    header[32] = (unsigned char)(block_align & 0xFF);
    header[33] = (unsigned char)(block_align >> 8);
    header[34] = 16; header[35] = 0;                /* bits per sample */

    memcpy(header + 36, "data", 4);
    header[40] = 0xFF; header[41] = 0xFF; header[42] = 0xFF; header[43] = 0xFF;

    write_all(fd, header, sizeof(header));
}

/* Audio capture child: reads the PCM device and pushes it down the pipe. */
static void capture_loop(snd_pcm_t *handle, int pipe_write_fd)
{
    char buffer[READ_BUFFER];
    const int bytes_per_frame = (int)(g_channels * 2);
    const snd_pcm_uframes_t frames_to_read = READ_BUFFER / bytes_per_frame;
    unsigned long xruns = 0;

    while (g_running) {
        snd_pcm_sframes_t got = snd_pcm_readi(handle, buffer, frames_to_read);

        if (got < 0) {
            if (got == -EPIPE) {
                xruns++;
                log_msg("capture overrun (%lu), recovering", xruns);
                snd_pcm_prepare(handle);
                continue;
            }
            if (got == -ESTRPIPE) {
                while (snd_pcm_resume(handle) == -EAGAIN)
                    usleep(100000);
                snd_pcm_prepare(handle);
                continue;
            }
            if (got == -EINTR)
                continue;
            log_msg("capture read failed: %s", snd_strerror((int)got));
            break;
        }
        if (got == 0)
            continue;

        if (write_all(pipe_write_fd, buffer, (size_t)got * bytes_per_frame) != 0)
            break;
    }

    snd_pcm_close(handle);
    close(pipe_write_fd);
    _exit(0);
}

static const char *http_path(const char *request, char *method, size_t method_len)
{
    const char *sp;
    size_t n;

    sp = strchr(request, ' ');
    if (!sp)
        return NULL;

    n = (size_t)(sp - request);
    if (n >= method_len)
        n = method_len - 1;
    memcpy(method, request, n);
    method[n] = '\0';

    sp++;
    if (*sp != '/')
        return NULL;

    n = strcspn(sp, " \r\n");
    {
        static char path[REQUEST_BUFFER];
        if (n >= sizeof(path))
            n = sizeof(path) - 1;
        memcpy(path, sp, n);
        path[n] = '\0';
        return path;
    }
}

static void send_simple(int fd, int code, const char *reason,
                        const char *content_type, const char *body, size_t body_len)
{
    char head[512];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\n"
                     "Content-Type: %s\r\n"
                     "Content-Length: %zu\r\n"
                     "Cache-Control: no-store\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Connection: close\r\n"
                     "\r\n",
                     code, reason, content_type, body_len);

    if (n > 0)
        write_all(fd, head, (size_t)n);
    if (body && body_len)
        write_all(fd, body, body_len);
}

static void send_stream_headers(int fd, unsigned int rate, unsigned int channels)
{
    char head[512];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: audio/wav\r\n"
                     "Cache-Control: no-store\r\n"
                     "Access-Control-Allow-Origin: *\r\n"
                     "Connection: close\r\n"
                     "\r\n");

    if (n > 0)
        write_all(fd, head, (size_t)n);
    write_wav_header(fd, rate, channels);
}

/*
 * Handshake written by the capture child to the parent before any audio:
 * the negotiated rate and channel count, or rate == 0 if the device could
 * not be opened. Keeps the WAV header honest without opening ALSA in a
 * process that then forks (ALSA handles are not fork-safe).
 */
struct capture_header {
    uint32_t rate;
    uint32_t channels;
};

static int read_full(int fd, void *buf, size_t len)
{
    char *p = buf;
    size_t left = len;

    while (left > 0) {
        ssize_t n = read(fd, p, left);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        p += n;
        left -= (size_t)n;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    int server_fd = -1;
    int pipe_fd[2] = { -1, -1 };
    pid_t capture_pid = -1;
    unsigned int port = DEFAULT_PORT;
    unsigned int period_ms = DEFAULT_PERIOD_MS;
    const char *address = DEFAULT_ADDRESS;
    int daemon_mode = 0;
    int client_fds[MAX_CLIENTS];
    unsigned long served_clients = 0;
    time_t started = time(NULL);
    struct sockaddr_in addr;
    struct sigaction sa;
    struct capture_header hdr;
    int capture_failed = 0;
    int i, opt, long_index = 0;

    for (i = 0; i < MAX_CLIENTS; i++)
        client_fds[i] = -1;

    static const struct option long_opts[] = {
        { "port",     required_argument, NULL, 'p' },
        { "address",  required_argument, NULL, 'a' },
        { "device",   required_argument, NULL, 'D' },
        { "rate",     required_argument, NULL, 'r' },
        { "channels", required_argument, NULL, 'c' },
        { "period",   required_argument, NULL, 'b' },
        { "daemon",   no_argument,       NULL, 'd' },
        { "verbose",  no_argument,       NULL, 'v' },
        { "version",  no_argument,       NULL, 'V' },
        { "help",     no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    while ((opt = getopt_long(argc, argv, "p:a:D:r:c:b:dvVh", long_opts, &long_index)) != -1) {
        switch (opt) {
        case 'p':
            port = (unsigned int)strtoul(optarg, NULL, 10);
            break;
        case 'a':
            address = optarg;
            break;
        case 'D':
            g_device = optarg;
            break;
        case 'r':
            g_rate = (unsigned int)strtoul(optarg, NULL, 10);
            break;
        case 'c':
            g_channels = (unsigned int)strtoul(optarg, NULL, 10);
            break;
        case 'b':
            period_ms = (unsigned int)strtoul(optarg, NULL, 10);
            break;
        case 'd':
            daemon_mode = 1;
            break;
        case 'v':
            g_verbose = 1;
            break;
        case 'V':
            printf("%s %s\n", PROGRAM, VERSION);
            return EXIT_SUCCESS;
        case 'h':
            printf("Usage: %s [options]\n\n"
                   "  -p, --port N       TCP port to listen on (default %d)\n"
                   "  -a, --address A    bind address (default %s)\n"
                   "  -D, --device D     ALSA capture device (default %s)\n"
                   "  -r, --rate N       sample rate in Hz (default %d)\n"
                   "  -c, --channels N   channel count (default %d)\n"
                   "  -b, --period N     ALSA period in ms (default %d)\n"
                   "  -d, --daemon       detach and run in the background\n"
                   "  -v, --verbose      log activity to stderr\n"
                   "  -V, --version      print version and exit\n"
                   "  -h, --help         this message\n\n"
                   "Endpoints: / (WAV stream), /healthz (JSON status)\n",
                   PROGRAM, DEFAULT_PORT, DEFAULT_ADDRESS, DEFAULT_DEVICE,
                   DEFAULT_RATE, DEFAULT_CHANNELS, DEFAULT_PERIOD_MS);
            return EXIT_SUCCESS;
        default:
            fprintf(stderr, "Try '%s --help'.\n", PROGRAM);
            return EXIT_FAILURE;
        }
    }

    if (port == 0 || port > 65535 || g_rate == 0 || g_channels == 0) {
        fprintf(stderr, "%s: invalid port/rate/channels\n", PROGRAM);
        return EXIT_FAILURE;
    }

    /* A disconnected client must not kill the daemon. */
    signal(SIGPIPE, SIG_IGN);

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    if (daemon_mode)
        daemonize();

    if (pipe(pipe_fd) != 0) {
        fprintf(stderr, "%s: pipe: %s\n", PROGRAM, strerror(errno));
        return EXIT_FAILURE;
    }

    capture_pid = fork();
    if (capture_pid < 0) {
        fprintf(stderr, "%s: fork: %s\n", PROGRAM, strerror(errno));
        return EXIT_FAILURE;
    }

    if (capture_pid == 0) {
        snd_pcm_t *handle;
        unsigned int rate = g_rate;

        close(pipe_fd[0]);

        /* ALSA handles are not fork-safe: the device is opened here, in the
         * process that will use it, and the result is reported to the parent
         * over the pipe before any audio flows. */
        handle = capture_open(&rate, period_ms);

        hdr.rate = handle ? rate : 0;
        hdr.channels = (uint32_t)g_channels;
        if (write_all(pipe_fd[1], &hdr, sizeof(hdr)) != 0) {
            if (handle)
                snd_pcm_close(handle);
            _exit(1);
        }
        if (!handle)
            _exit(1);

        g_rate = rate;
        capture_loop(handle, pipe_fd[1]);
    }

    close(pipe_fd[1]);

    if (read_full(pipe_fd[0], &hdr, sizeof(hdr)) != 0 || hdr.rate == 0) {
        fprintf(stderr, "%s: capture device %s is not usable\n",
                PROGRAM, g_device);
        close(pipe_fd[0]);
        kill(capture_pid, SIGTERM);
        waitpid(capture_pid, NULL, 0);
        return EXIT_FAILURE;
    }

    g_rate = hdr.rate;
    g_channels = hdr.channels;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        fprintf(stderr, "%s: socket: %s\n", PROGRAM, strerror(errno));
        goto shutdown;
    }

    opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, address, &addr.sin_addr) != 1) {
        fprintf(stderr, "%s: invalid bind address '%s'\n", PROGRAM, address);
        goto shutdown;
    }

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "%s: bind %s:%u: %s\n", PROGRAM, address, port, strerror(errno));
        goto shutdown;
    }

    if (listen(server_fd, MAX_CLIENTS) < 0) {
        fprintf(stderr, "%s: listen: %s\n", PROGRAM, strerror(errno));
        goto shutdown;
    }

    log_msg("listening on %s:%u (%s, %u Hz, %u ch)", address, port,
            g_device, g_rate, (unsigned int)g_channels);
    if (!daemon_mode)
        printf("Streaming %s at %u Hz mono on http://%s:%u/\n",
               g_device, g_rate, address, port);

    while (g_running) {
        fd_set read_fds;
        struct timeval timeout = { 1, 0 };
        int max_fd = server_fd;
        int activity;
        char audio_buffer[READ_BUFFER];
        ssize_t bytes;

        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);
        FD_SET(pipe_fd[0], &read_fds);
        if (pipe_fd[0] > max_fd)
            max_fd = pipe_fd[0];

        for (i = 0; i < MAX_CLIENTS; i++) {
            if (client_fds[i] >= 0) {
                FD_SET(client_fds[i], &read_fds);
                if (client_fds[i] > max_fd)
                    max_fd = client_fds[i];
            }
        }

        activity = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);
        if (activity < 0) {
            if (errno == EINTR)
                continue;
            log_msg("select failed: %s", strerror(errno));
            break;
        }
        if (activity == 0)
            continue;   /* idle tick; lets g_running be re-checked */

        if (FD_ISSET(server_fd, &read_fds)) {
            struct sockaddr_in peer;
            socklen_t peer_len = sizeof(peer);
            struct timeval send_timeout = { SEND_TIMEOUT_S, 0 };
            char request[REQUEST_BUFFER];
            char method[16];
            const char *path;
            int client_fd;
            ssize_t got;

            client_fd = accept(server_fd, (struct sockaddr *)&peer, &peer_len);
            if (client_fd < 0) {
                if (errno != EINTR && errno != EAGAIN)
                    log_msg("accept failed: %s", strerror(errno));
                continue;
            }

            setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));

            /* Peek at the request line so /healthz and HEAD can be served. */
            {
                struct pollfd pfd = { client_fd, POLLIN, 0 };
                memset(request, 0, sizeof(request));
                got = 0;
                if (poll(&pfd, 1, REQUEST_TIMEOUT_MS) > 0)
                    got = recv(client_fd, request, sizeof(request) - 1, 0);
            }

            if (got > 0 && (path = http_path(request, method, sizeof(method))) != NULL) {
                if (strcmp(path, "/healthz") == 0 || strcmp(path, "/health") == 0) {
                    char body[256];
                    int live = 0;
                    int n;

                    for (i = 0; i < MAX_CLIENTS; i++)
                        if (client_fds[i] >= 0)
                            live++;

                    n = snprintf(body, sizeof(body),
                                 "{\"status\":\"ok\",\"device\":\"%s\",\"rate\":%u,"
                                 "\"channels\":%u,\"clients\":%d,\"served\":%lu,"
                                 "\"uptime_s\":%ld}\n",
                                 g_device, g_rate, (unsigned int)g_channels,
                                 live, served_clients, (long)(time(NULL) - started));
                    send_simple(client_fd, 200, "OK", "application/json",
                                body, (size_t)n);
                    close(client_fd);
                    continue;
                }
                if (strcmp(method, "HEAD") == 0) {
                    send_stream_headers(client_fd, g_rate, (unsigned int)g_channels);
                    close(client_fd);
                    continue;
                }
            }

            /* Stream client: hand it the header and the WAV preamble. */
            {
                int slot = -1;
                for (i = 0; i < MAX_CLIENTS; i++) {
                    if (client_fds[i] < 0) {
                        slot = i;
                        break;
                    }
                }
                if (slot < 0) {
                    send_simple(client_fd, 503, "Service Unavailable",
                                "text/plain", "too many clients\n", 17);
                    close(client_fd);
                    continue;
                }
                send_stream_headers(client_fd, g_rate, (unsigned int)g_channels);
                client_fds[slot] = client_fd;
                served_clients++;
                log_msg("client connected from %s:%u (slot %d, %lu served)",
                        inet_ntoa(peer.sin_addr), (unsigned int)ntohs(peer.sin_port),
                        slot, served_clients);
            }
        }

        if (FD_ISSET(pipe_fd[0], &read_fds)) {
            bytes = read(pipe_fd[0], audio_buffer, sizeof(audio_buffer));
            if (bytes <= 0) {
                log_msg("capture pipe closed, shutting down");
                capture_failed = 1;
                break;
            }
            for (i = 0; i < MAX_CLIENTS; i++) {
                if (client_fds[i] < 0)
                    continue;
                if (write_all(client_fds[i], audio_buffer, (size_t)bytes) != 0) {
                    log_msg("dropping client in slot %d (%s)", i, strerror(errno));
                    close(client_fds[i]);
                    client_fds[i] = -1;
                }
            }
        }

        for (i = 0; i < MAX_CLIENTS; i++) {
            char junk[256];
            if (client_fds[i] < 0 || !FD_ISSET(client_fds[i], &read_fds))
                continue;
            if (read(client_fds[i], junk, sizeof(junk)) <= 0) {
                close(client_fds[i]);
                client_fds[i] = -1;
                log_msg("client in slot %d disconnected", i);
            }
        }
    }

shutdown:
    if (server_fd >= 0)
        close(server_fd);
    for (i = 0; i < MAX_CLIENTS; i++)
        if (client_fds[i] >= 0)
            close(client_fds[i]);
    if (pipe_fd[0] >= 0)
        close(pipe_fd[0]);
    if (capture_pid > 0) {
        kill(capture_pid, SIGTERM);
        waitpid(capture_pid, NULL, 0);
    }
    log_msg("stopped");
    return capture_failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
