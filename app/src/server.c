#include "server.h"

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <libgen.h>
#include <stdio.h>
#include <SDL2/SDL_thread.h>
#include <SDL2/SDL_timer.h>
#include <SDL2/SDL_platform.h>

#include "config.h"
#include "command.h"
#include "util/lock.h"
#include "util/log.h"
#include "util/net.h"
#include "util/str_util.h"

#define SOCKET_NAME "scrcpy"
#define SERVER_FILENAME "scrcpy-server"

#define DEFAULT_SERVER_PATH PREFIX "/share/scrcpy/" SERVER_FILENAME
#define DEVICE_SERVER_PATH "/data/local/tmp/scrcpy-server.jar"

static char *
get_server_path(void) {
#ifdef __WINDOWS__
    const wchar_t *server_path_env = _wgetenv(L"SCRCPY_SERVER_PATH");
#else
    const char *server_path_env = getenv("SCRCPY_SERVER_PATH");
#endif
    if (server_path_env) {
        // if the envvar is set, use it
#ifdef __WINDOWS__
        char *server_path = utf8_from_wide_char(server_path_env);
#else
        char *server_path = SDL_strdup(server_path_env);
#endif
        if (!server_path) {
            LOGE("Could not allocate memory");
            return NULL;
        }
        LOGD("Using SCRCPY_SERVER_PATH: %s", server_path);
        return server_path;
    }

#ifndef PORTABLE
    LOGD("Using server: " DEFAULT_SERVER_PATH);
    char *server_path = SDL_strdup(DEFAULT_SERVER_PATH);
    if (!server_path) {
        LOGE("Could not allocate memory");
        return NULL;
    }
    // the absolute path is hardcoded
    return server_path;
#else

    // use scrcpy-server in the same directory as the executable
    char *executable_path = get_executable_path();
    if (!executable_path) {
        LOGE("Could not get executable path, "
             "using " SERVER_FILENAME " from current directory");
        // not found, use current directory
        return SERVER_FILENAME;
    }
    char *dir = dirname(executable_path);
    size_t dirlen = strlen(dir);

    // sizeof(SERVER_FILENAME) gives statically the size including the null byte
    size_t len = dirlen + 1 + sizeof(SERVER_FILENAME);
    char *server_path = SDL_malloc(len);
    if (!server_path) {
        LOGE("Could not alloc server path string, "
             "using " SERVER_FILENAME " from current directory");
        SDL_free(executable_path);
        return SERVER_FILENAME;
    }

    memcpy(server_path, dir, dirlen);
    server_path[dirlen] = PATH_SEPARATOR;
    memcpy(&server_path[dirlen + 1], SERVER_FILENAME, sizeof(SERVER_FILENAME));
    // the final null byte has been copied with SERVER_FILENAME

    SDL_free(executable_path);

    LOGD("Using server (portable): %s", server_path);
    return server_path;
#endif
}

static bool
push_server(const char *serial) {
    char *server_path = get_server_path();
    if (!server_path) {
        return false;
    }
    if (!is_regular_file(server_path)) {
        LOGE("'%s' does not exist or is not a regular file\n", server_path);
        SDL_free(server_path);
        return false;
    }
    process_t process = adb_push(serial, server_path, DEVICE_SERVER_PATH);
    SDL_free(server_path);
    return process_check_success(process, "adb push");
}

static bool
enable_tunnel_reverse(const char *serial, uint16_t local_port) {
    process_t process = adb_reverse(serial, SOCKET_NAME, local_port);
    return process_check_success(process, "adb reverse");
}

static bool
disable_tunnel_reverse(const char *serial) {
    process_t process = adb_reverse_remove(serial, SOCKET_NAME);
    return process_check_success(process, "adb reverse --remove");
}

static bool
enable_tunnel_forward(const char *serial, uint16_t local_port) {
    process_t process = adb_forward(serial, local_port, SOCKET_NAME);
    return process_check_success(process, "adb forward");
}

static bool
disable_tunnel_forward(const char *serial, uint16_t local_port) {
    process_t process = adb_forward_remove(serial, local_port);
    return process_check_success(process, "adb forward --remove");
}

static bool
disable_tunnel(struct server *server) {
    if (server->tunnel_forward) {
        return disable_tunnel_forward(server->serial, server->local_port);
    }
    return disable_tunnel_reverse(server->serial);
}

static socket_t
listen_on_port(uint16_t port) {
#define IPV4_LOCALHOST 0x7F000001
    return net_listen(IPV4_LOCALHOST, port, 1);
}

static bool
enable_tunnel_reverse_any_port(struct server *server,
                               struct sc_port_range port_range) {
    uint16_t port = port_range.first;
    for (;;) {
        if (!enable_tunnel_reverse(server->serial, port)) {
            // the command itself failed, it will fail on any port
            return false;
        }

        // At the application level, the device part is "the server" because it
        // serves video stream and control. However, at the network level, the
        // client listens and the server connects to the client. That way, the
        // client can listen before starting the server app, so there is no
        // need to try to connect until the server socket is listening on the
        // device.
        server->server_socket = listen_on_port(port);
        if (server->server_socket != INVALID_SOCKET) {
            // success
            server->local_port = port;
            return true;
        }

        // failure, disable tunnel and try another port
        if (!disable_tunnel_reverse(server->serial)) {
            LOGW("Could not remove reverse tunnel on port %" PRIu16, port);
        }

        // check before incrementing to avoid overflow on port 65535
        if (port < port_range.last) {
            LOGW("Could not listen on port %" PRIu16", retrying on %" PRIu16,
                 port, (uint16_t) (port + 1));
            port++;
            continue;
        }

        if (port_range.first == port_range.last) {
            LOGE("Could not listen on port %" PRIu16, port_range.first);
        } else {
            LOGE("Could not listen on any port in range %" PRIu16 ":%" PRIu16,
                 port_range.first, port_range.last);
        }
        return false;
    }
}

static bool
enable_tunnel_forward_any_port(struct server *server,
                               struct sc_port_range port_range) {
    server->tunnel_forward = true;
    uint16_t port = port_range.first;
    for (;;) {
        if (enable_tunnel_forward(server->serial, port)) {
            // success
            server->local_port = port;
            return true;
        }

        if (port < port_range.last) {
            LOGW("Could not forward port %" PRIu16", retrying on %" PRIu16,
                 port, (uint16_t) (port + 1));
            port++;
            continue;
        }

        if (port_range.first == port_range.last) {
            LOGE("Could not forward port %" PRIu16, port_range.first);
        } else {
            LOGE("Could not forward any port in range %" PRIu16 ":%" PRIu16,
                 port_range.first, port_range.last);
        }
        return false;
    }
}

static bool
enable_tunnel_any_port(struct server *server, struct sc_port_range port_range,
                       bool force_adb_forward) {
    if (!force_adb_forward) {
        // Attempt to use "adb reverse"
        if (enable_tunnel_reverse_any_port(server, port_range)) {
            return true;
        }

        // if "adb reverse" does not work (e.g. over "adb connect"), it
        // fallbacks to "adb forward", so the app socket is the client

        LOGW("'adb reverse' failed, fallback to 'adb forward'");
    }

    return enable_tunnel_forward_any_port(server, port_range);
}

static const char *
log_level_to_server_string(enum sc_log_level level) {
    switch (level) {
        case SC_LOG_LEVEL_DEBUG:
            return "debug";
        case SC_LOG_LEVEL_INFO:
            return "info";
        case SC_LOG_LEVEL_WARN:
            return "warn";
        case SC_LOG_LEVEL_ERROR:
            return "error";
        default:
            assert(!"unexpected log level");
            return "(unknown)";
    }
}

static process_t
execute_server_adb(struct server *server, const struct server_params *params) {
    char max_size_string[6];
    char bit_rate_string[11];
    char max_fps_string[6];
    char lock_video_orientation_string[5];
    char display_id_string[6];
    sprintf(max_size_string, "%"PRIu16, params->max_size);
    sprintf(bit_rate_string, "%"PRIu32, params->bit_rate);
    sprintf(max_fps_string, "%"PRIu16, params->max_fps);
    sprintf(lock_video_orientation_string, "%"PRIi8, params->lock_video_orientation);
    sprintf(display_id_string, "%"PRIu16, params->display_id);
    const char *const cmd[] = {
        "shell",
        "CLASSPATH=" DEVICE_SERVER_PATH,
        "app_process",
#ifdef SERVER_DEBUGGER
# define SERVER_DEBUGGER_PORT "5005"
# ifdef SERVER_DEBUGGER_METHOD_NEW
        /* Android 9 and above */
        "-XjdwpProvider:internal -XjdwpOptions:transport=dt_socket,suspend=y,server=y,address="
# else
        /* Android 8 and below */
        "-agentlib:jdwp=transport=dt_socket,suspend=y,server=y,address="
# endif
            SERVER_DEBUGGER_PORT,
#endif
        "/", // unused
        "com.genymobile.scrcpy.Server",
        SCRCPY_VERSION,
        log_level_to_server_string(params->log_level),
        max_size_string,
        bit_rate_string,
        max_fps_string,
        lock_video_orientation_string,
        server->tunnel_forward ? "true" : "false",
        params->crop ? params->crop : "-",
        "true", // always send frame meta (packet boundaries + timestamp)
        params->control ? "true" : "false",
        display_id_string,
        params->show_touches ? "true" : "false",
        params->stay_awake ? "true" : "false",
        params->codec_options ? params->codec_options : "-",
        params->encoder_name ? params->encoder_name : "-",
    };
#ifdef SERVER_DEBUGGER
    LOGI("Server debugger waiting for a client on device port "
         SERVER_DEBUGGER_PORT "...");
    // From the computer, run
    //     adb forward tcp:5005 tcp:5005
    // Then, from Android Studio: Run > Debug > Edit configurations...
    // On the left, click on '+', "Remote", with:
    //     Host: localhost
    //     Port: 5005
    // Then click on "Debug"
#endif
    return adb_execute(server->serial, cmd, sizeof(cmd) / sizeof(cmd[0]));
}

static enum process_result
execute_server_curl(struct server *server, const struct server_params *params) {
    char max_size_string[6];
    char bit_rate_string[11];
    char max_fps_string[6];
    char lock_video_orientation_string[5];
    char display_id_string[6];
    char url[1024];
    sprintf(max_size_string, "%"PRIu16, params->max_size);
    sprintf(bit_rate_string, "%"PRIu32, params->bit_rate);
    sprintf(max_fps_string, "%"PRIu16, params->max_fps);
    sprintf(lock_video_orientation_string, "%"PRIi8, params->lock_video_orientation);
    sprintf(display_id_string, "%"PRIu16, params->display_id);
    snprintf(url, sizeof(url), 
        "%s/startScrcpy/%s/%s/%s/%s/%s/%s/%s/%s/%s/%s/%s/%s/%s/%s/%s", 
        server->url, 
        SCRCPY_VERSION,
        log_level_to_server_string(params->log_level),
        max_size_string,
        bit_rate_string,
        max_fps_string,
        lock_video_orientation_string,
        "true",
        params->crop ? params->crop : "-",
        "true", // always send frame meta (packet boundaries + timestamp)
        params->control ? "true" : "false",
        display_id_string,
        params->show_touches ? "true" : "false",
        params->stay_awake ? "true" : "false",
        params->codec_options ? params->codec_options : "-",
        params->encoder_name ? params->encoder_name : "-");

    LOGI("%s\n", url);

    char buffer[1024];
    int ret = curl_get(url, buffer, sizeof(buffer));
    if (ret > 0 && strstr(buffer, "success")) {
        if((size_t)ret >= sizeof(buffer)) {
            buffer[sizeof(buffer)-1] = '\0';
        }else{
            buffer[ret] = '\0';
        }
        LOGI("%s\n", buffer);
        if (ret > 0 && strstr(buffer, "success")) {
            return PROCESS_SUCCESS;
        }
    }
    return PROCESS_ERROR_GENERIC;
}

static enum process_result
stop_server_curl(struct server *server) {
    char url[1024];
    snprintf(url, sizeof(url), 
        "%s/stopScrcpy/", 
        server->url);
    LOGI("%s\n", url);

    char buffer[1024];
    int ret = curl_get(url, buffer, sizeof(buffer));
    if (ret > 0 && strstr(buffer, "success")) {
        if((size_t)ret >= sizeof(buffer)) {
            buffer[sizeof(buffer)-1] = '\0';
        }else{
            buffer[ret] = '\0';
        }
        LOGI("%s\n", buffer);
        if (ret > 0 && strstr(buffer, "success")) {
            return PROCESS_SUCCESS;
        }
    }
    return PROCESS_ERROR_GENERIC;
}

static socket_t
connect_and_read_byte(uint32_t addr, uint16_t port) {
    socket_t socket = net_connect(addr, port);
    if (socket == INVALID_SOCKET) {
        return INVALID_SOCKET;
    }

    char byte;
    // the connection may succeed even if the server behind the "adb tunnel"
    // is not listening, so read one byte to detect a working connection
    if (net_recv(socket, &byte, 1) != 1) {
        // the server is not listening yet behind the adb tunnel
        net_close(socket);
        return INVALID_SOCKET;
    }
    return socket;
}

static socket_t
connect_to_server(uint32_t addr, uint16_t port, uint32_t attempts, uint32_t delay) {
    do {
        LOGD("Remaining connection attempts: %d", (int) attempts);
        socket_t socket = connect_and_read_byte(addr, port);
        if (socket != INVALID_SOCKET) {
            // it worked!
            return socket;
        }
        if (attempts) {
            SDL_Delay(delay);
        }
    } while (--attempts > 0);
    return INVALID_SOCKET;
}

static void
close_socket(socket_t socket) {
    assert(socket != INVALID_SOCKET);
    net_shutdown(socket, SHUT_RDWR);
    if (!net_close(socket)) {
        LOGW("Could not close socket");
    }
}

bool
server_init(struct server *server) {
    server->serial = NULL;
    server->url = NULL;
    server->addr = 0;
    server->process = PROCESS_NONE;
    server->wait_server_thread = NULL;
    atomic_flag_clear_explicit(&server->server_socket_closed,
                               memory_order_relaxed);

    server->mutex = SDL_CreateMutex();
    if (!server->mutex) {
        return false;
    }

    server->process_terminated_cond = SDL_CreateCond();
    if (!server->process_terminated_cond) {
        SDL_DestroyMutex(server->mutex);
        return false;
    }

    server->process_terminated = false;

    server->server_socket = INVALID_SOCKET;
    server->video_socket = INVALID_SOCKET;
    server->control_socket = INVALID_SOCKET;

    server->port_range.first = 0;
    server->port_range.last = 0;
    server->local_port = 0;

    server->tunnel_enabled = false;
    server->tunnel_forward = false;
    server->direct = false;

    return true;
}

static int
run_wait_server(void *data) {
    struct server *server = data;
    cmd_simple_wait(server->process, NULL); // ignore exit code

    mutex_lock(server->mutex);
    server->process_terminated = true;
    cond_signal(server->process_terminated_cond);
    mutex_unlock(server->mutex);

    // no need for synchronization, server_socket is initialized before this
    // thread was created
    if (server->server_socket != INVALID_SOCKET
            && !atomic_flag_test_and_set(&server->server_socket_closed)) {
        // On Linux, accept() is unblocked by shutdown(), but on Windows, it is
        // unblocked by closesocket(). Therefore, call both (close_socket()).
        close_socket(server->server_socket);
    }
    LOGD("Server terminated");
    return 0;
}

bool
server_start(struct server *server, const char *serial,
             const struct server_params *params) {
    server->port_range = params->port_range;

    if (serial) {
        server->serial = SDL_strdup(serial);
        if (!server->serial) {
            return false;
        }
    }

    if (server->direct) {
        // server will connect to our server socket
        if (execute_server_curl(server, params) != PROCESS_SUCCESS) {
            goto error2;
        }
    }else {
        if (!push_server(serial)) {
            goto error1;
        }

        if (!enable_tunnel_any_port(server, params->port_range,
                                    params->force_adb_forward)) {
            goto error1;
        }

        // server will connect to our server socket
        server->process = execute_server_adb(server, params);
        if (server->process == PROCESS_NONE) {
            goto error2;
        }
    }

    // If the server process dies before connecting to the server socket, then
    // the client will be stuck forever on accept(). To avoid the problem, we
    // must be able to wake up the accept() call when the server dies. To keep
    // things simple and multiplatform, just spawn a new thread waiting for the
    // server process and calling shutdown()/close() on the server socket if
    // necessary to wake up any accept() blocking call.
    server->wait_server_thread =
        SDL_CreateThread(run_wait_server, "wait-server", server);
    if (!server->wait_server_thread) {
        cmd_terminate(server->process);
        cmd_simple_wait(server->process, NULL); // ignore exit code
        goto error2;
    }

    server->tunnel_enabled = true;

    return true;

error2:
    if (!server->tunnel_forward) {
        bool was_closed =
            atomic_flag_test_and_set(&server->server_socket_closed);
        // the thread is not started, the flag could not be already set
        assert(!was_closed);
        (void) was_closed;
        close_socket(server->server_socket);
    }
    if (server->direct) {
        stop_server_curl(server);
    }else{
        disable_tunnel(server);
    }
error1:
    SDL_free(server->serial);
    return false;
}

bool
server_connect_to(struct server *server) {
    if(server->direct) {
        uint32_t attempts = 12;
        uint32_t delay = 1000; // ms
        server->video_socket =
            connect_to_server(server->addr, server->port_range.first, attempts, delay);
        if (server->video_socket == INVALID_SOCKET) {
            return false;
        }

        // we know that the device is listening, we don't need several attempts
        server->control_socket =
            net_connect(server->addr, server->port_range.first);
        if (server->control_socket == INVALID_SOCKET) {
            return false;
        }
        
        return true;
    } else if (server->tunnel_forward) {
        uint32_t attempts = 100;
        uint32_t delay = 100; // ms
        server->video_socket =
            connect_to_server(IPV4_LOCALHOST, server->local_port, attempts, delay);
        if (server->video_socket == INVALID_SOCKET) {
            return false;
        }

        // we know that the device is listening, we don't need several attempts
        server->control_socket =
            net_connect(IPV4_LOCALHOST, server->local_port);
        if (server->control_socket == INVALID_SOCKET) {
            return false;
        }

        // we don't need the adb tunnel anymore
        disable_tunnel(server); // ignore failure
        server->tunnel_enabled = false;

        return true;
    } else {
        server->video_socket = net_accept(server->server_socket);
        if (server->video_socket == INVALID_SOCKET) {
            return false;
        }

        server->control_socket = net_accept(server->server_socket);
        if (server->control_socket == INVALID_SOCKET) {
            // the video_socket will be cleaned up on destroy
            return false;
        }

        // we don't need the server socket anymore
        if (!atomic_flag_test_and_set(&server->server_socket_closed)) {
            // close it from here
            close_socket(server->server_socket);
            // otherwise, it is closed by run_wait_server()
        }

        return true;
    }
}

void
server_stop(struct server *server) {
    if (server->server_socket != INVALID_SOCKET
            && !atomic_flag_test_and_set(&server->server_socket_closed)) {
        close_socket(server->server_socket);
    }
    if (server->video_socket != INVALID_SOCKET) {
        close_socket(server->video_socket);
    }
    if (server->control_socket != INVALID_SOCKET) {
        close_socket(server->control_socket);
    }

    assert(server->process != PROCESS_NONE);

    if (server->tunnel_enabled && !server->direct) {
        // ignore failure
        disable_tunnel(server);
    }

    if (server->direct) {
        stop_server_curl(server);
    }

    // Give some delay for the server to terminate properly
    mutex_lock(server->mutex);
    int r = 0;
    if (!server->process_terminated) {
#define WATCHDOG_DELAY_MS 1000
        r = cond_wait_timeout(server->process_terminated_cond,
                              server->mutex,
                              WATCHDOG_DELAY_MS);
    }
    mutex_unlock(server->mutex);

    // After this delay, kill the server if it's not dead already.
    // On some devices, closing the sockets is not sufficient to wake up the
    // blocking calls while the device is asleep.
    if (r == SDL_MUTEX_TIMEDOUT) {
        // FIXME There is a race condition here: there is a small chance that
        // the process is already terminated, and the PID assigned to a new
        // process.
        LOGW("Killing the server...");
        cmd_terminate(server->process);
    }

    SDL_WaitThread(server->wait_server_thread, NULL);
}

void
server_destroy(struct server *server) {
    SDL_free(server->serial);
    SDL_free(server->url);
    SDL_DestroyCond(server->process_terminated_cond);
    SDL_DestroyMutex(server->mutex);
}
