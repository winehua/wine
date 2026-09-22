/* Cross-bitness process and IPC gate for the WineHua Steam baseline. */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../winehua_smoke_protocol.h"

#define IPC_MAGIC 0x57484950u
#define IPC_TIMEOUT_MS 15000
#define IPC_ROUNDS 2

struct shared_payload
{
    DWORD magic;
    DWORD round;
    DWORD parent_pid;
    DWORD child_pid;
    char parent_arch[16];
    char child_arch[16];
};

struct process_metrics
{
    unsigned int create_process_count;
    unsigned int child_exit_count;
    BOOL named_pipe;
    BOOL shared_memory;
    BOOL named_event;
    BOOL inherited_handle;
    BOOL unquoted_image_path;
    BOOL loopback;
    BOOL auto_reset_event;
    BOOL thread_callback;
    BOOL critical_section;
    DWORD last_error;
    char failed_stage[64];
    char peer_arch[16];
};

struct thread_probe
{
    HANDLE wake;
    HANDLE done;
    CRITICAL_SECTION lock;
    LONG counter;
};

static DWORD WINAPI probe_thread(void *arg)
{
    struct thread_probe *probe = arg;
    unsigned int i, j;

    for (i = 0; i < 2; ++i)
    {
        if (WaitForSingleObject(probe->wake, IPC_TIMEOUT_MS) != WAIT_OBJECT_0) return 1;
        for (j = 0; j < 256; ++j)
        {
            EnterCriticalSection(&probe->lock);
            ++probe->counter;
            LeaveCriticalSection(&probe->lock);
        }
        if (!SetEvent(probe->done)) return 2;
    }
    return 0;
}

static BOOL test_thread_and_auto_reset(struct process_metrics *metrics)
{
    struct thread_probe probe = {0};
    HANDLE thread = NULL;
    DWORD exit_code = STILL_ACTIVE;
    unsigned int i, j;
    BOOL passed = FALSE;

    probe.wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    probe.done = CreateEventA(NULL, FALSE, FALSE, NULL);
    if (!probe.wake || !probe.done) goto done;
    InitializeCriticalSection(&probe.lock);
    thread = CreateThread(NULL, 0, probe_thread, &probe, 0, NULL);
    if (!thread) goto done_lock;

    for (i = 0; i < 2; ++i)
    {
        if (!SetEvent(probe.wake)) goto done_lock;
        for (j = 0; j < 256; ++j)
        {
            EnterCriticalSection(&probe.lock);
            ++probe.counter;
            LeaveCriticalSection(&probe.lock);
        }
        if (WaitForSingleObject(probe.done, IPC_TIMEOUT_MS) != WAIT_OBJECT_0)
            goto done_lock;
        if (WaitForSingleObject(probe.done, 0) != WAIT_TIMEOUT) goto done_lock;
    }
    metrics->auto_reset_event = TRUE;
    if (WaitForSingleObject(thread, IPC_TIMEOUT_MS) != WAIT_OBJECT_0 ||
        !GetExitCodeThread(thread, &exit_code) || exit_code) goto done_lock;
    metrics->thread_callback = TRUE;
    metrics->critical_section = probe.counter == 1024;
    passed = metrics->critical_section;

done_lock:
    if (!passed && thread && exit_code == STILL_ACTIVE)
    {
        SetEvent(probe.wake);
        if (WaitForSingleObject(thread, IPC_TIMEOUT_MS) == WAIT_TIMEOUT)
        {
            SetEvent(probe.wake);
            if (WaitForSingleObject(thread, IPC_TIMEOUT_MS) == WAIT_TIMEOUT)
                TerminateThread(thread, ERROR_TIMEOUT);
        }
    }
    if (thread) CloseHandle(thread);
    DeleteCriticalSection(&probe.lock);
done:
    if (!passed) metrics->last_error = GetLastError();
    if (probe.wake) CloseHandle(probe.wake);
    if (probe.done) CloseHandle(probe.done);
    return passed;
}

static const char *pe_architecture(void)
{
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

static const char *argument_value(int argc, char **argv, const char *name)
{
    int i;
    for (i = 1; i + 1 < argc; ++i)
        if (!lstrcmpiA(argv[i], name)) return argv[i + 1];
    return NULL;
}

static BOOL has_argument(int argc, char **argv, const char *name)
{
    int i;
    for (i = 1; i < argc; ++i)
        if (!lstrcmpiA(argv[i], name)) return TRUE;
    return FALSE;
}

static BOOL test_unquoted_image_path(DWORD *last_error)
{
    STARTUPINFOA startup = {sizeof(startup)};
    PROCESS_INFORMATION process = {0};
    char source[MAX_PATH] = {0}, directory[MAX_PATH] = {0}, target[MAX_PATH] = {0};
    char command[2 * MAX_PATH] = {0};
    DWORD exit_code = STILL_ACTIVE;
    BOOL ret = FALSE;
    int length;

    if (!GetModuleFileNameA(NULL, source, sizeof(source))) goto done;
    if (!GetTempPathA(sizeof(directory), directory)) goto done;
    if (strlen(directory) + strlen("WineHua Process Path") + 1 >= sizeof(directory))
    {
        SetLastError(ERROR_BUFFER_OVERFLOW);
        goto done;
    }
    lstrcatA(directory, "WineHua Process Path");
    if (!CreateDirectoryA(directory, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) goto done;
    length = snprintf(target, sizeof(target), "%s\\space child.exe", directory);
    if (length < 0 || length >= (int)sizeof(target) || !CopyFileA(source, target, FALSE)) goto done;
    length = snprintf(command, sizeof(command), "%s --space-path-child --token preserved", target);
    if (length < 0 || length >= (int)sizeof(command) ||
        !CreateProcessA(target, command, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, directory,
                        &startup, &process)) goto done;
    if (WaitForSingleObject(process.hProcess, IPC_TIMEOUT_MS) != WAIT_OBJECT_0)
    {
        SetLastError(ERROR_TIMEOUT);
        goto done;
    }
    if (!GetExitCodeProcess(process.hProcess, &exit_code)) goto done;
    if (exit_code)
    {
        SetLastError(exit_code);
        goto done;
    }
    ret = TRUE;

done:
    if (!ret) *last_error = GetLastError();
    if (process.hThread) CloseHandle(process.hThread);
    if (process.hProcess) CloseHandle(process.hProcess);
    if (target[0]) DeleteFileA(target);
    if (directory[0]) RemoveDirectoryA(directory);
    return ret;
}

static BOOL read_exact(HANDLE handle, void *buffer, DWORD size)
{
    BYTE *cursor = buffer;
    while (size)
    {
        DWORD transferred = 0;
        if (!ReadFile(handle, cursor, size, &transferred, NULL) || !transferred) return FALSE;
        cursor += transferred;
        size -= transferred;
    }
    return TRUE;
}

static BOOL write_exact(HANDLE handle, const void *buffer, DWORD size)
{
    const BYTE *cursor = buffer;
    while (size)
    {
        DWORD transferred = 0;
        if (!WriteFile(handle, cursor, size, &transferred, NULL) || !transferred) return FALSE;
        cursor += transferred;
        size -= transferred;
    }
    return TRUE;
}

static BOOL overlapped_transfer(HANDLE pipe, BOOL write_operation, void *buffer, DWORD size)
{
    OVERLAPPED overlapped;
    DWORD transferred = 0;
    BOOL ret;

    memset(&overlapped, 0, sizeof(overlapped));
    overlapped.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!overlapped.hEvent) return FALSE;
    if (write_operation)
        ret = WriteFile(pipe, buffer, size, &transferred, &overlapped);
    else
        ret = ReadFile(pipe, buffer, size, &transferred, &overlapped);
    if (!ret && GetLastError() == ERROR_IO_PENDING)
    {
        if (WaitForSingleObject(overlapped.hEvent, IPC_TIMEOUT_MS) == WAIT_OBJECT_0)
            ret = GetOverlappedResult(pipe, &overlapped, &transferred, FALSE);
        else
        {
            CancelIo(pipe);
            ret = FALSE;
            SetLastError(ERROR_TIMEOUT);
        }
    }
    CloseHandle(overlapped.hEvent);
    return ret && transferred == size;
}

static int child_main(int argc, char **argv)
{
    const char *pipe_name = argument_value(argc, argv, "--pipe");
    const char *mapping_name = argument_value(argc, argv, "--mapping");
    const char *event_name = argument_value(argc, argv, "--event");
    const char *round_text = argument_value(argc, argv, "--round");
    const char *inherited_text = argument_value(argc, argv, "--inherited-event");
    HANDLE pipe = INVALID_HANDLE_VALUE, mapping = NULL, event = NULL, inherited_event = NULL;
    struct shared_payload *shared = NULL;
    char request[32], response[32];
    DWORD round;
    int ret = 20;

    if (!pipe_name || !mapping_name || !event_name || !round_text || !inherited_text) return 21;
    round = strtoul(round_text, NULL, 10);
    inherited_event = (HANDLE)(ULONG_PTR)_strtoui64(inherited_text, NULL, 10);
    if (!inherited_event || WaitForSingleObject(inherited_event, 0) == WAIT_FAILED) return 22;

    pipe = CreateFileA(pipe_name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
    if (pipe == INVALID_HANDLE_VALUE) return 23;
    mapping = OpenFileMappingA(FILE_MAP_ALL_ACCESS, FALSE, mapping_name);
    if (!mapping) { ret = 24; goto done; }
    shared = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*shared));
    if (!shared) { ret = 25; goto done; }
    if (shared->magic != IPC_MAGIC || shared->round != round ||
        lstrcmpA(shared->parent_arch, argument_value(argc, argv, "--parent-arch")))
    {
        ret = 26;
        goto done;
    }

    event = OpenEventA(EVENT_MODIFY_STATE, FALSE, event_name);
    if (!event) { ret = 27; goto done; }
    memset(request, 0, sizeof(request));
    if (!read_exact(pipe, request, sizeof(request)) ||
        snprintf(response, sizeof(response), "child:%s:%lu", pe_architecture(), (unsigned long)round) < 0 ||
        strncmp(request, "parent:", 7) || !write_exact(pipe, response, sizeof(response)))
    {
        ret = 28;
        goto done;
    }

    shared->child_pid = GetCurrentProcessId();
    lstrcpynA(shared->child_arch, pe_architecture(), sizeof(shared->child_arch));
    FlushViewOfFile(shared, sizeof(*shared));
    if (!SetEvent(event) || !SetEvent(inherited_event)) { ret = 29; goto done; }
    ret = 0;

done:
    if (event) CloseHandle(event);
    if (shared) UnmapViewOfFile(shared);
    if (mapping) CloseHandle(mapping);
    if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
    return ret;
}

static BOOL run_ipc_round(const char *peer_exe, DWORD round, struct process_metrics *metrics)
{
    SECURITY_ATTRIBUTES inherit_attributes = {sizeof(inherit_attributes), NULL, TRUE};
    STARTUPINFOA startup = {sizeof(startup)};
    PROCESS_INFORMATION process;
    OVERLAPPED connect_overlapped;
    struct shared_payload *shared = NULL;
    HANDLE pipe = INVALID_HANDLE_VALUE, mapping = NULL, named_event = NULL;
    HANDLE inherited_event = NULL, connect_event = NULL;
    char pipe_name[128], mapping_name[128], event_name[128];
    char command[1024], request[32], response[32], expected[32];
    DWORD error = ERROR_SUCCESS, exit_code = STILL_ACTIVE;
    BOOL connected = FALSE, connect_pending = FALSE, ret = FALSE;

    memset(&process, 0, sizeof(process));
    memset(&connect_overlapped, 0, sizeof(connect_overlapped));
    snprintf(pipe_name, sizeof(pipe_name), "\\\\.\\pipe\\winehua-%lu-%lu",
             (unsigned long)GetCurrentProcessId(), (unsigned long)round);
    snprintf(mapping_name, sizeof(mapping_name), "Local\\winehua-map-%lu-%lu",
             (unsigned long)GetCurrentProcessId(), (unsigned long)round);
    snprintf(event_name, sizeof(event_name), "Local\\winehua-event-%lu-%lu",
             (unsigned long)GetCurrentProcessId(), (unsigned long)round);

    pipe = CreateNamedPipeA(pipe_name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, 256, 256,
                            IPC_TIMEOUT_MS, NULL);
    mapping = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, 0,
                                 sizeof(*shared), mapping_name);
    named_event = CreateEventA(NULL, TRUE, FALSE, event_name);
    inherited_event = CreateEventA(&inherit_attributes, TRUE, FALSE, NULL);
    connect_event = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (pipe == INVALID_HANDLE_VALUE || !mapping || !named_event || !inherited_event || !connect_event)
    {
        error = GetLastError();
        lstrcpynA(metrics->failed_stage, "resource-create", sizeof(metrics->failed_stage));
        goto done;
    }
    shared = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(*shared));
    if (!shared)
    {
        error = GetLastError();
        lstrcpynA(metrics->failed_stage, "mapping-view", sizeof(metrics->failed_stage));
        goto done;
    }
    memset(shared, 0, sizeof(*shared));
    shared->magic = IPC_MAGIC;
    shared->round = round;
    shared->parent_pid = GetCurrentProcessId();
    lstrcpynA(shared->parent_arch, pe_architecture(), sizeof(shared->parent_arch));

    connect_overlapped.hEvent = connect_event;
    connected = ConnectNamedPipe(pipe, &connect_overlapped);
    if (!connected)
    {
        error = GetLastError();
        if (error != ERROR_IO_PENDING && error != ERROR_PIPE_CONNECTED)
        {
            lstrcpynA(metrics->failed_stage, "pipe-listen", sizeof(metrics->failed_stage));
            goto done;
        }
        connect_pending = error == ERROR_IO_PENDING;
    }
    snprintf(command, sizeof(command),
             "\"%s\" --child --pipe \"%s\" --mapping \"%s\" --event \"%s\" "
             "--round %lu --parent-arch %s --inherited-event %llu",
             peer_exe, pipe_name, mapping_name, event_name, (unsigned long)round,
             pe_architecture(), (unsigned long long)(ULONG_PTR)inherited_event);
    if (!CreateProcessA(NULL, command, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL,
                        &startup, &process))
    {
        error = GetLastError();
        lstrcpynA(metrics->failed_stage, "create-process", sizeof(metrics->failed_stage));
        goto done;
    }
    metrics->create_process_count++;

    if (connect_pending)
    {
        if (WaitForSingleObject(connect_event, IPC_TIMEOUT_MS) != WAIT_OBJECT_0 ||
            !GetOverlappedResult(pipe, &connect_overlapped, &error, FALSE))
        {
            error = GetLastError();
            lstrcpynA(metrics->failed_stage, "pipe-connect", sizeof(metrics->failed_stage));
            goto done;
        }
    }
    snprintf(request, sizeof(request), "parent:%s:%lu", pe_architecture(), (unsigned long)round);
    memset(response, 0, sizeof(response));
    if (!overlapped_transfer(pipe, TRUE, request, sizeof(request)) ||
        !overlapped_transfer(pipe, FALSE, response, sizeof(response)))
    {
        error = GetLastError();
        lstrcpynA(metrics->failed_stage, "named-pipe", sizeof(metrics->failed_stage));
        goto done;
    }
    snprintf(expected, sizeof(expected), "child:%s:%lu",
             winehua_smoke_env("WINEHUA_PEER_ARCH", "unknown"), (unsigned long)round);
    if (lstrcmpA(response, expected))
    {
        error = ERROR_BAD_FORMAT;
        lstrcpynA(metrics->failed_stage, "peer-architecture", sizeof(metrics->failed_stage));
        goto done;
    }
    metrics->named_pipe = TRUE;
    if (WaitForSingleObject(named_event, IPC_TIMEOUT_MS) != WAIT_OBJECT_0)
    {
        error = ERROR_TIMEOUT;
        lstrcpynA(metrics->failed_stage, "named-event", sizeof(metrics->failed_stage));
        goto done;
    }
    metrics->named_event = TRUE;
    if (WaitForSingleObject(inherited_event, IPC_TIMEOUT_MS) != WAIT_OBJECT_0)
    {
        error = ERROR_TIMEOUT;
        lstrcpynA(metrics->failed_stage, "handle-inheritance", sizeof(metrics->failed_stage));
        goto done;
    }
    metrics->inherited_handle = TRUE;
    if (!shared->child_pid || lstrcmpA(shared->child_arch,
                                      winehua_smoke_env("WINEHUA_PEER_ARCH", "unknown")))
    {
        error = ERROR_INVALID_DATA;
        lstrcpynA(metrics->failed_stage, "shared-memory", sizeof(metrics->failed_stage));
        goto done;
    }
    metrics->shared_memory = TRUE;
    if (WaitForSingleObject(process.hProcess, IPC_TIMEOUT_MS) != WAIT_OBJECT_0 ||
        !GetExitCodeProcess(process.hProcess, &exit_code) || exit_code)
    {
        error = exit_code == STILL_ACTIVE ? ERROR_TIMEOUT : exit_code;
        lstrcpynA(metrics->failed_stage, "child-reap", sizeof(metrics->failed_stage));
        goto done;
    }
    metrics->child_exit_count++;
    ret = TRUE;

done:
    if (!ret && process.hProcess)
    {
        TerminateProcess(process.hProcess, 99);
        WaitForSingleObject(process.hProcess, 3000);
    }
    if (process.hThread) CloseHandle(process.hThread);
    if (process.hProcess) CloseHandle(process.hProcess);
    if (pipe != INVALID_HANDLE_VALUE) { DisconnectNamedPipe(pipe); CloseHandle(pipe); }
    if (shared) UnmapViewOfFile(shared);
    if (mapping) CloseHandle(mapping);
    if (named_event) CloseHandle(named_event);
    if (inherited_event) CloseHandle(inherited_event);
    if (connect_event) CloseHandle(connect_event);
    metrics->last_error = error;
    return ret;
}

static BOOL socket_write_exact(SOCKET socket_handle, const char *data, int length)
{
    while (length)
    {
        int sent = send(socket_handle, data, length, 0);
        if (sent <= 0) return FALSE;
        data += sent;
        length -= sent;
    }
    return TRUE;
}

static BOOL socket_read_exact(SOCKET socket_handle, char *data, int length)
{
    while (length)
    {
        int received = recv(socket_handle, data, length, 0);
        if (received <= 0) return FALSE;
        data += received;
        length -= received;
    }
    return TRUE;
}

static BOOL test_loopback(DWORD *last_error)
{
    WSADATA data;
    SOCKET listener = INVALID_SOCKET, client = INVALID_SOCKET, server = INVALID_SOCKET;
    struct sockaddr_in address;
    int address_length = sizeof(address);
    DWORD timeout = 5000;
    char request[8] = "request", response[8] = {0};
    BOOL ret = FALSE;

    if (WSAStartup(MAKEWORD(2, 2), &data)) { *last_error = WSAGetLastError(); return FALSE; }
    listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) goto done;
    setsockopt(listener, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
    setsockopt(listener, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, (struct sockaddr *)&address, sizeof(address)) || listen(listener, 1) ||
        getsockname(listener, (struct sockaddr *)&address, &address_length)) goto done;
    client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (client == INVALID_SOCKET || connect(client, (struct sockaddr *)&address, sizeof(address))) goto done;
    server = accept(listener, NULL, NULL);
    if (server == INVALID_SOCKET) goto done;
    if (!socket_write_exact(client, request, sizeof(request)) ||
        !socket_read_exact(server, response, sizeof(response)) || memcmp(request, response, sizeof(request)) ||
        !socket_write_exact(server, "response", 8) ||
        !socket_read_exact(client, response, sizeof(response)) || memcmp(response, "response", 8)) goto done;
    ret = TRUE;

done:
    if (!ret) *last_error = WSAGetLastError();
    if (server != INVALID_SOCKET) closesocket(server);
    if (client != INVALID_SOCKET) closesocket(client);
    if (listener != INVALID_SOCKET) closesocket(listener);
    WSACleanup();
    return ret;
}

int main(int argc, char **argv)
{
    struct winehua_smoke_options smoke;
    struct process_metrics metrics;
    const char *peer_exe, *peer_arch;
    char json[1024];
    DWORD round;
    BOOL passed = TRUE;

    if (has_argument(argc, argv, "--child")) return child_main(argc, argv);
    if (has_argument(argc, argv, "--space-path-child"))
        return !argument_value(argc, argv, "--token") ||
               lstrcmpA(argument_value(argc, argv, "--token"), "preserved");
    if (!winehua_smoke_parse_options(&smoke, argc, argv, 0)) return 2;
    memset(&metrics, 0, sizeof(metrics));
    peer_exe = winehua_smoke_env("WINEHUA_PEER_EXE", "");
    peer_arch = winehua_smoke_env("WINEHUA_PEER_ARCH", "unknown");
    lstrcpynA(metrics.peer_arch, peer_arch, sizeof(metrics.peer_arch));
    winehua_smoke_write_result(&smoke, "STARTED", "startup", "process and IPC gate starting", "{}");
    if (!peer_exe[0] || !lstrcmpA(peer_arch, "unknown"))
    {
        metrics.last_error = ERROR_BAD_ENVIRONMENT;
        lstrcpynA(metrics.failed_stage, "configuration", sizeof(metrics.failed_stage));
        passed = FALSE;
    }
    for (round = 1; passed && round <= IPC_ROUNDS; ++round)
        passed = run_ipc_round(peer_exe, round, &metrics);
    if (passed)
    {
        metrics.unquoted_image_path = test_unquoted_image_path(&metrics.last_error);
        if (!metrics.unquoted_image_path)
        {
            lstrcpynA(metrics.failed_stage, "unquoted-image-path", sizeof(metrics.failed_stage));
            passed = FALSE;
        }
    }
    if (passed)
    {
        metrics.loopback = test_loopback(&metrics.last_error);
        if (!metrics.loopback)
        {
            lstrcpynA(metrics.failed_stage, "loopback", sizeof(metrics.failed_stage));
            passed = FALSE;
        }
    }
    if (passed && !test_thread_and_auto_reset(&metrics))
    {
        lstrcpynA(metrics.failed_stage, "thread-auto-reset", sizeof(metrics.failed_stage));
        passed = FALSE;
    }
    if (metrics.create_process_count != IPC_ROUNDS || metrics.child_exit_count != IPC_ROUNDS)
        passed = FALSE;
    snprintf(json, sizeof(json),
             "{\"parentArchitecture\":\"%s\",\"peerArchitecture\":\"%s\","
             "\"createProcessCount\":%u,\"childExitCount\":%u,\"namedPipeDuplex\":%s,"
             "\"sharedMemory\":%s,\"eventSynchronization\":%s,\"handleInheritance\":%s,"
             "\"unquotedImagePath\":%s,\"loopback\":%s,\"autoResetEvent\":%s,"
             "\"threadCallback\":%s,\"criticalSection\":%s,\"lastError\":%lu,\"failedStage\":\"%s\"}",
             pe_architecture(), metrics.peer_arch, metrics.create_process_count,
             metrics.child_exit_count, metrics.named_pipe ? "true" : "false",
             metrics.shared_memory ? "true" : "false", metrics.named_event ? "true" : "false",
             metrics.inherited_handle ? "true" : "false",
             metrics.unquoted_image_path ? "true" : "false", metrics.loopback ? "true" : "false",
             metrics.auto_reset_event ? "true" : "false",
             metrics.thread_callback ? "true" : "false",
             metrics.critical_section ? "true" : "false",
             (unsigned long)metrics.last_error, metrics.failed_stage);
    winehua_smoke_write_result(&smoke, passed ? "PASS" : "FAIL",
                               passed ? "complete" : metrics.failed_stage,
                               passed ? "cross-bitness process and IPC gate passed" :
                                        "process or IPC capability failed", json);
    return passed ? 0 : 1;
}
