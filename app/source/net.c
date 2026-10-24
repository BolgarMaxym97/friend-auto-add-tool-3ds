#include "net.h"

#include <3ds.h>
#include <curl/curl.h>
#include <malloc.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define SOC_BUFFER_SIZE 0x100000

static u32 *soc_buffer;

typedef struct {
    char *buf;
    size_t cap, len;
    bool too_big;
} Sink;

bool net_init(void)
{
    soc_buffer = memalign(0x1000, SOC_BUFFER_SIZE);
    if (!soc_buffer)
        return false;
    if (R_FAILED(socInit(soc_buffer, SOC_BUFFER_SIZE))) {
        free(soc_buffer);
        soc_buffer = NULL;
        return false;
    }
    curl_global_init(CURL_GLOBAL_DEFAULT);
    return true;
}

void net_exit(void)
{
    if (!soc_buffer)
        return;
    curl_global_cleanup();
    socExit();
    free(soc_buffer);
    soc_buffer = NULL;
}

static size_t sink_write(char *data, size_t size, size_t n, void *user)
{
    Sink *s = user;
    size_t bytes = size * n;
    if (s->len + bytes > s->cap) {
        s->too_big = true;
        return 0; /* curl aborts with CURLE_WRITE_ERROR */
    }
    memcpy(s->buf + s->len, data, bytes);
    s->len += bytes;
    return bytes;
}

static bool transient(CURLcode res)
{
    return res == CURLE_SSL_CONNECT_ERROR || res == CURLE_COULDNT_CONNECT || res == CURLE_COULDNT_RESOLVE_HOST ||
           res == CURLE_OPERATION_TIMEDOUT || res == CURLE_RECV_ERROR || res == CURLE_SEND_ERROR;
}

bool net_get(const char *url, char *buf, size_t cap, size_t *len, char *err, size_t err_cap)
{
    Sink sink = {buf, cap, 0, false};
    char detail[CURL_ERROR_SIZE];
    *len = 0;
    if (!soc_buffer) {
        snprintf(err, err_cap, "мережа не запущена");
        return false;
    }
    CURL *curl = curl_easy_init();
    if (!curl) {
        snprintf(err, err_cap, "curl не запустився");
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "FriendAutoAddTool/1.0 (Nintendo 3DS)");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
    curl_easy_setopt(curl, CURLOPT_CAINFO, CA_BUNDLE);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 40L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sink_write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, detail);

    /* the New 3DS once failed the TLS handshake on a shaky connection: one more go */
    CURLcode res = CURLE_OK;
    for (int attempt = 0; attempt < 2; attempt++) {
        if (attempt)
            svcSleepThread(2000ull * 1000 * 1000);
        detail[0] = 0;
        sink.len = 0;
        sink.too_big = false;
        res = curl_easy_perform(curl);
        if (!transient(res))
            break;
    }
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if (sink.too_big) {
        snprintf(err, err_cap, "файл бази більший за %u КБ", (unsigned)(cap / 1024));
        return false;
    }
    if (res != CURLE_OK) {
        if (res == CURLE_HTTP_RETURNED_ERROR)
            snprintf(err, err_cap, "сервер відповів %ld", status);
        else if (detail[0])
            snprintf(err, err_cap, "%s (%s)", curl_easy_strerror(res), detail);
        else
            snprintf(err, err_cap, "%s", curl_easy_strerror(res));
        return false;
    }
    *len = sink.len;
    return true;
}
