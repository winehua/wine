#ifndef WINEHUA_SMOKE_PROTOCOL_H
#define WINEHUA_SMOKE_PROTOCOL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

struct winehua_smoke_options
{
    BOOL automation;
    BOOL offscreen;
    BOOL present;
    DWORD seconds;
    char run_id[96];
    char test_id[96];
    char result_path[MAX_PATH];
};

static ULONGLONG winehua_smoke_timestamp_ms(void)
{
    FILETIME file_time;
    ULARGE_INTEGER value;
    GetSystemTimeAsFileTime(&file_time);
    value.LowPart = file_time.dwLowDateTime;
    value.HighPart = file_time.dwHighDateTime;
    return value.QuadPart / 10000ULL - 11644473600000ULL;
}

static void winehua_smoke_copy_arg(char *dst, size_t dst_size, const char *src)
{
    if (!dst_size) return;
    lstrcpynA(dst, src ? src : "", (int)dst_size);
}

static BOOL winehua_smoke_parse_options(struct winehua_smoke_options *options,
                                        int argc, char **argv, DWORD default_seconds)
{
    int i;
    memset(options, 0, sizeof(*options));
    options->seconds = default_seconds;
    options->present = TRUE;

    for (i = 1; i < argc; ++i)
    {
        if (!lstrcmpiA(argv[i], "--automation")) options->automation = TRUE;
        else if (!lstrcmpiA(argv[i], "--offscreen")) { options->offscreen = TRUE; options->present = FALSE; }
        else if (!lstrcmpiA(argv[i], "--present")) { options->present = TRUE; options->offscreen = FALSE; }
        else if (!lstrcmpiA(argv[i], "--run-id") && i + 1 < argc)
            winehua_smoke_copy_arg(options->run_id, sizeof(options->run_id), argv[++i]);
        else if (!lstrcmpiA(argv[i], "--test-id") && i + 1 < argc)
            winehua_smoke_copy_arg(options->test_id, sizeof(options->test_id), argv[++i]);
        else if (!lstrcmpiA(argv[i], "--result") && i + 1 < argc)
            winehua_smoke_copy_arg(options->result_path, sizeof(options->result_path), argv[++i]);
        else if (!lstrcmpiA(argv[i], "--seconds") && i + 1 < argc)
        {
            int seconds = atoi(argv[++i]);
            if (seconds > 0 && seconds <= 3600) options->seconds = (DWORD)seconds;
        }
    }

    if (!options->run_id[0]) winehua_smoke_copy_arg(options->run_id, sizeof(options->run_id), "manual");
    if (!options->test_id[0]) winehua_smoke_copy_arg(options->test_id, sizeof(options->test_id), "unknown");
    return !options->automation || options->result_path[0] != '\0';
}

static BOOL winehua_smoke_ensure_parent(const char *path)
{
    char copy[MAX_PATH];
    char *cursor;
    winehua_smoke_copy_arg(copy, sizeof(copy), path);
    for (cursor = copy; *cursor; ++cursor)
    {
        if ((*cursor == '\\' || *cursor == '/') && cursor > copy + 2)
        {
            char saved = *cursor;
            *cursor = '\0';
            if (!CreateDirectoryA(copy, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return FALSE;
            *cursor = saved;
        }
    }
    return TRUE;
}

static void winehua_smoke_write_json_string(FILE *file, const char *value)
{
    const unsigned char *cursor = (const unsigned char *)(value ? value : "");
    fputc('"', file);
    while (*cursor)
    {
        switch (*cursor)
        {
        case '"': fputs("\\\"", file); break;
        case '\\': fputs("\\\\", file); break;
        case '\n': fputs("\\n", file); break;
        case '\r': fputs("\\r", file); break;
        case '\t': fputs("\\t", file); break;
        default:
            if (*cursor < 0x20) fprintf(file, "\\u%04x", *cursor);
            else fputc(*cursor, file);
            break;
        }
        ++cursor;
    }
    fputc('"', file);
}

static const char *winehua_smoke_env(const char *name, const char *fallback)
{
    const char *value = getenv(name);
    return value && value[0] ? value : fallback;
}

static BOOL winehua_smoke_write_result(const struct winehua_smoke_options *options,
                                       const char *status, const char *stage,
                                       const char *message, const char *metrics_json)
{
    char temporary[MAX_PATH];
    FILE *file;
    const char *pe_architecture;

    if (!options->result_path[0]) return !options->automation;
    if (!winehua_smoke_ensure_parent(options->result_path)) return FALSE;
    snprintf(temporary, sizeof(temporary), "%s.tmp.%lu", options->result_path,
             (unsigned long)GetCurrentProcessId());
    file = fopen(temporary, "wb");
    if (!file) return FALSE;

#ifdef _WIN64
    pe_architecture = "x86_64";
#else
    pe_architecture = "x86";
#endif
    fputs("{\n  \"schemaVersion\": 1,\n  \"runId\": ", file);
    winehua_smoke_write_json_string(file, options->run_id);
    fputs(",\n  \"testId\": ", file);
    winehua_smoke_write_json_string(file, options->test_id);
    fputs(",\n  \"status\": ", file);
    winehua_smoke_write_json_string(file, status);
    fputs(",\n  \"stage\": ", file);
    winehua_smoke_write_json_string(file, stage);
    fputs(",\n  \"message\": ", file);
    winehua_smoke_write_json_string(file, message);
    fprintf(file, ",\n  \"pid\": %lu,\n  \"heartbeatTimestampMs\": %llu,\n",
            (unsigned long)GetCurrentProcessId(), winehua_smoke_timestamp_ms());
    fputs("  \"architecture\": {\n    \"peArchitecture\": ", file);
    winehua_smoke_write_json_string(file, pe_architecture);
    fputs(",\n    \"wineUnixArchitecture\": ", file);
    winehua_smoke_write_json_string(file, winehua_smoke_env("WINEHUA_WINE_UNIX_ARCH", "unknown"));
    fputs(",\n    \"vulkanLoaderArchitecture\": ", file);
    winehua_smoke_write_json_string(file, winehua_smoke_env("WINEHUA_VULKAN_LOADER_ARCH", "unavailable"));
    fputs(",\n    \"venusIcdArchitecture\": ", file);
    winehua_smoke_write_json_string(file, winehua_smoke_env("WINEHUA_VENUS_ICD_ARCH", "unavailable"));
    fputs(",\n    \"hostArchitecture\": ", file);
    winehua_smoke_write_json_string(file, winehua_smoke_env("WINEHUA_HOST_ARCH", "unknown"));
#ifdef _WIN64
    fputs(",\n    \"wow64ThunkEnabled\": false", file);
#else
    fputs(",\n    \"wow64ThunkEnabled\": true", file);
#endif
    fputs(",\n    \"box64Enabled\": ", file);
    fputs(winehua_smoke_env("USE_LIBBOX64", "0")[0] == '1' ? "true" : "false", file);
    fputs("\n  },\n  \"metrics\": ", file);
    fputs(metrics_json && metrics_json[0] ? metrics_json : "{}", file);
    fputs("\n}\n", file);

    if (fflush(file))
    {
        fclose(file);
        DeleteFileA(temporary);
        return FALSE;
    }
    if (fclose(file))
    {
        DeleteFileA(temporary);
        return FALSE;
    }
    if (!MoveFileExA(temporary, options->result_path,
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    {
        DeleteFileA(temporary);
        return FALSE;
    }
    return TRUE;
}

#endif /* WINEHUA_SMOKE_PROTOCOL_H */
