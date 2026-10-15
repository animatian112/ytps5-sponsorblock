/*
 * ps5-native-app-boilerplate - libcurl support for PS5 native titles.
 * Copyright (C) 2026 BlackBearReloaded
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What PacBrew's libcurl 8.18.0 / OpenSSL 3.5.2 archives need to run in a
 * native title; see console_curl.h. Taken from ProsperoRadio (elevated) and
 * ProsperoLichess (sandboxed), where each piece was found on hardware. When a
 * PacBrew update brings new undefined symbols, the linker names them; a symbol
 * that links but resolves to libScePosixForWebKit is a null import at run time
 * and needs a definition here too.
 */

#include "console_curl.h"
#include "yt/dns.h"

#include <arpa/inet.h>
#include <errno.h>
#include <langinfo.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/time.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

extern int sceNetPoolCreate(const char *name, int size, int flags);
extern int sceNetPoolDestroy(int pool);
extern int sceNetResolverCreate(const char *name, int pool, int flags);
extern int sceNetResolverStartNtoa(int resolver, const char *hostname, struct in_addr *address,
                                   int timeout_us, int retries, int flags);
extern int sceNetResolverDestroy(int resolver);
extern const char *sceKernelGetFsSandboxRandomWord(void);

/* ---- Certificate list ---------------------------------------------------------------------- */

static pthread_mutex_t console_curl_ca_lock = PTHREAD_MUTEX_INITIALIZER;
static char console_curl_ca_path[160];

static int console_curl_readable(const char *path)
{
    struct stat facts;
    return stat(path, &facts) == 0 && S_ISREG(facts.st_mode);
}

const char *console_curl_ca_file(void)
{
    pthread_mutex_lock(&console_curl_ca_lock);
    if (console_curl_ca_path[0] == '\0' || !console_curl_readable(console_curl_ca_path))
    {
        snprintf(console_curl_ca_path, sizeof(console_curl_ca_path),
                 "/system/common/cert/CA_LIST.cer");
        if (!console_curl_readable(console_curl_ca_path))
        {
            const char *word = sceKernelGetFsSandboxRandomWord();
            snprintf(console_curl_ca_path, sizeof(console_curl_ca_path),
                     "/%s/common/cert/CA_LIST.cer", word != NULL ? word : "");
        }
    }
    pthread_mutex_unlock(&console_curl_ca_lock);
    return console_curl_ca_path;
}

/* ---- Per-handle setup ---------------------------------------------------------------------- */

enum
{
    CONSOLE_SO_NBIO = 0x1200 /* the console's own non-blocking socket option */
};

int console_curl_nonblocking(int socket)
{
    const int on = 1;
    return setsockopt(socket, SOL_SOCKET, CONSOLE_SO_NBIO, &on, sizeof(on));
}

static int console_curl_on_socket(void *user, curl_socket_t socket, curlsocktype purpose)
{
    (void)user;
    (void)purpose;
    /* Keep going if it fails: the transfer still works, only slowly. */
    (void)console_curl_nonblocking(socket);
    return CURL_SOCKOPT_OK;
}

/* The Mozilla root store, compiled in (ca_bundle.c). The console's own list at
 * /system/common/cert/CA_LIST.cer is not readable from every firmware's sandbox
 * (13.60: every request failed, the home feed stayed empty), and curl refuses
 * all HTTPS when its CA file can't be loaded. The built-in list always loads;
 * the system file is added only when it is actually there. */
extern const char console_curl_ca_bundle[];
extern const size_t console_curl_ca_bundle_len;

void console_curl_setup(CURL *easy)
{
    struct curl_blob roots;
    const char *system_file = console_curl_ca_file();
    roots.data = (void *)console_curl_ca_bundle;
    roots.len = console_curl_ca_bundle_len;
    roots.flags = CURL_BLOB_NOCOPY;
    (void)curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    (void)curl_easy_setopt(easy, CURLOPT_CAINFO_BLOB, &roots);
    (void)curl_easy_setopt(easy, CURLOPT_CAINFO,
                           console_curl_readable(system_file) ? system_file : NULL);
    (void)curl_easy_setopt(easy, CURLOPT_CAPATH, NULL);
    (void)curl_easy_setopt(easy, CURLOPT_SOCKOPTFUNCTION, console_curl_on_socket);
}

/* ---- fcntl on sockets ---------------------------------------------------------------------- */

/* In a sandboxed title the console's libc refuses fcntl on sockets with EINVAL.
 * curl then fails every connect ("fcntl set CLOEXEC: Invalid argument") and its
 * sockets stay blocking. Linked with --wrap=fcntl, every fcntl call in the app
 * and the archives comes here; only a refused call is treated as a socket. */

extern int __real_fcntl(int descriptor, int command, ...);

static int console_curl_socket_flags(int socket, int command, int value)
{
    switch (command)
    {
    case F_GETFD:
    case F_SETFD:
        return 0; /* a title never execs another program */
    case F_SETFL:
    {
        const int on = (value & O_NONBLOCK) != 0;
        return setsockopt(socket, SOL_SOCKET, CONSOLE_SO_NBIO, &on, sizeof(on));
    }
    case F_GETFL:
    {
        int on = 0;
        socklen_t length = sizeof(on);
        if (getsockopt(socket, SOL_SOCKET, CONSOLE_SO_NBIO, &on, &length) < 0)
            return -1;
        return O_RDWR | (on ? O_NONBLOCK : 0);
    }
    default:
        errno = EINVAL;
        return -1;
    }
}

int __wrap_fcntl(int descriptor, int command, ...)
{
    va_list arguments;
    va_start(arguments, command);
    /* Every command curl and its libraries use takes an int or a pointer. */
    const intptr_t argument = va_arg(arguments, intptr_t);
    va_end(arguments);
    const int result = __real_fcntl(descriptor, command, argument);
    if (result >= 0 || errno != EINVAL)
        return result;
    if (command == F_GETFD || command == F_SETFD || command == F_SETFL || command == F_GETFL)
        return console_curl_socket_flags(descriptor, command, (int)argument);
    return result;
}

/* ---- Name lookup --------------------------------------------------------------------------- */

/* The console's resolver (sceNetResolverStartNtoa): one IPv4 address. */
static int console_curl_system_lookup(const char *name, struct in_addr *address)
{
    const int pool = sceNetPoolCreate("console_curl_dns", 16 * 1024, 0);
    if (pool < 0)
        return EAI_MEMORY;
    int result = EAI_FAIL;
    const int resolver = sceNetResolverCreate("console_curl_dns", pool, 0);
    if (resolver >= 0)
    {
        /* 5 s per try, 2 retries. */
        result =
            sceNetResolverStartNtoa(resolver, name, address, 5000000, 2, 0) < 0 ? EAI_NONAME : 0;
        (void)sceNetResolverDestroy(resolver);
    }
    (void)sceNetPoolDestroy(pool);
    return result;
}

/* Our own lookup: an A query over UDP to public resolvers, read with yt/dns.c.
 * On firmware 13.60 the console's resolver hands back an address that refuses
 * connections for names behind CNAME aliases (www.youtube.com,
 * oauth2.googleapis.com) while direct names (www.google.com) work, so the
 * names go here first and the console's resolver is only the fallback. */
static const char *const console_curl_dns_servers[] = {"1.1.1.1", "8.8.8.8", "9.9.9.9"};
enum
{
    CONSOLE_DNS_SERVERS = 3,
    CONSOLE_DNS_MAX = 4,
    CONSOLE_DNS_CACHE = 16,
    CONSOLE_DNS_TTL = 300
};

struct console_dns_entry
{
    char name[128];
    uint32_t addresses[CONSOLE_DNS_MAX];
    int count;
    time_t stored;
};

static pthread_mutex_t console_dns_lock = PTHREAD_MUTEX_INITIALIZER;
static struct console_dns_entry console_dns_cache[CONSOLE_DNS_CACHE];
static int console_dns_next;
static int console_dns_unreachable; /* every public resolver timed out once */
static char console_dns_last_server[16];

/* The UDP exchange uses libSceNet directly (the calls the SponsorBlock tile
 * proved in a sandboxed title), not libc's socket layer. */
extern int sceNetSocket(const char *name, int family, int type, int protocol);
extern int sceNetSocketClose(int socket);
extern int sceNetSetsockopt(int socket, int level, int option, const void *value,
                            unsigned int size);
extern int sceNetSendto(int socket, const void *data, size_t length, int flags, const void *address,
                        unsigned int address_length);
extern int sceNetRecv(int socket, void *data, size_t length, int flags);

static uint16_t console_dns_id(void)
{
    static uint32_t counter;
    uintptr_t stack = (uintptr_t)&counter;
    counter = counter * 1103515245u + 12345u + (uint32_t)time(NULL) + (uint32_t)stack;
    return (uint16_t)(counter >> 8);
}

static int console_dns_query(const char *server, const char *name, uint32_t *addresses, int max)
{
    uint8_t packet[DNS_MAX_PACKET];
    pthread_mutex_lock(&console_dns_lock);
    const uint16_t id = console_dns_id();
    pthread_mutex_unlock(&console_dns_lock);
    const int length = dns_build_query(packet, sizeof(packet), id, name);
    if (length < 0)
        return -1;
    const int socket_fd = sceNetSocket("console_curl_dns", AF_INET, 2 /* DGRAM */, 0);
    if (socket_fd < 0)
        return -1;
    const int timeout_us = 1500000; /* 1.5 s per server */
    (void)sceNetSetsockopt(socket_fd, 0xffff, 0x1106 /* RCVTIMEO */, &timeout_us,
                           sizeof(timeout_us));
    (void)sceNetSetsockopt(socket_fd, 0xffff, 0x1105 /* SNDTIMEO */, &timeout_us,
                           sizeof(timeout_us));
    struct sockaddr_in to;
    memset(&to, 0, sizeof(to));
    to.sin_len = sizeof(to);
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    (void)inet_pton(AF_INET, server, &to.sin_addr);
    int found = -1;
    if (sceNetSendto(socket_fd, packet, (size_t)length, 0, &to, sizeof(to)) == length)
    {
        /* Skip stray datagrams (another id) a few times before giving up. */
        for (int tries = 0; tries < 4 && found == -1; ++tries)
        {
            const int got = sceNetRecv(socket_fd, packet, sizeof(packet), 0);
            if (got <= 0)
                break;
            found = dns_parse_a(packet, (size_t)got, id, addresses, max);
        }
    }
    (void)sceNetSocketClose(socket_fd);
    return found;
}

/* Fills up to CONSOLE_DNS_MAX addresses; returns how many (0: none found). */
static int console_curl_own_lookup(const char *name, uint32_t *addresses)
{
    const time_t now = time(NULL);
    pthread_mutex_lock(&console_dns_lock);
    for (int i = 0; i < CONSOLE_DNS_CACHE; ++i)
    {
        struct console_dns_entry *entry = &console_dns_cache[i];
        if (entry->count > 0 && strcmp(entry->name, name) == 0 &&
            now - entry->stored < CONSOLE_DNS_TTL)
        {
            memcpy(addresses, entry->addresses, sizeof(entry->addresses));
            const int count = entry->count;
            pthread_mutex_unlock(&console_dns_lock);
            return count;
        }
    }
    const int skip = console_dns_unreachable;
    pthread_mutex_unlock(&console_dns_lock);
    if (skip || strlen(name) >= sizeof(console_dns_cache[0].name))
        return 0;

    int count = 0;
    int answered = 0;
    for (int i = 0; i < CONSOLE_DNS_SERVERS && count <= 0; ++i)
    {
        count = console_dns_query(console_curl_dns_servers[i], name, addresses, CONSOLE_DNS_MAX);
        if (count != -1)
            answered = 1;
        if (count > 0)
        {
            pthread_mutex_lock(&console_dns_lock);
            snprintf(console_dns_last_server, sizeof(console_dns_last_server), "%s",
                     console_curl_dns_servers[i]);
            pthread_mutex_unlock(&console_dns_lock);
        }
    }
    pthread_mutex_lock(&console_dns_lock);
    if (!answered)
        console_dns_unreachable = 1; /* this network blocks them: stop waiting */
    if (count > 0)
    {
        struct console_dns_entry *entry = &console_dns_cache[console_dns_next];
        console_dns_next = (console_dns_next + 1) % CONSOLE_DNS_CACHE;
        snprintf(entry->name, sizeof(entry->name), "%s", name);
        memset(entry->addresses, 0, sizeof(entry->addresses));
        memcpy(entry->addresses, addresses, (size_t)count * sizeof(uint32_t));
        entry->count = count;
        entry->stored = now;
    }
    pthread_mutex_unlock(&console_dns_lock);
    return count > 0 ? count : 0;
}

/* The addresses for a name: ours first, then the console's resolver. */
static int console_curl_lookup(const char *name, uint32_t *addresses, int *count)
{
    struct in_addr address;
    *count = 0;
    if (inet_pton(AF_INET, name, &address) == 1)
    {
        addresses[0] = address.s_addr;
        *count = 1;
        return 0;
    }
    *count = console_curl_own_lookup(name, addresses);
    if (*count > 0)
        return 0;
    const int failed = console_curl_system_lookup(name, &address);
    if (failed != 0)
        return failed;
    addresses[0] = address.s_addr;
    *count = 1;
    return 0;
}

static void console_dns_format(uint32_t address, char *out, size_t size)
{
    const uint8_t *b = (const uint8_t *)&address; /* network order */
    snprintf(out, size, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

void console_curl_describe_lookup(const char *name, char *out, size_t size)
{
    uint32_t own[CONSOLE_DNS_MAX];
    struct in_addr system_address;
    char own_text[INET_ADDRSTRLEN] = "-", system_text[INET_ADDRSTRLEN] = "-";
    const int own_count = console_curl_own_lookup(name, own);
    if (own_count > 0)
        console_dns_format(own[0], own_text, sizeof(own_text));
    if (console_curl_system_lookup(name, &system_address) == 0)
        console_dns_format(system_address.s_addr, system_text, sizeof(system_text));
    pthread_mutex_lock(&console_dns_lock);
    snprintf(out, size, "DNS own %s%s%s%s, PS5 %s", own_text, own_count > 0 ? " (" : "",
             own_count > 0 ? console_dns_last_server : "", own_count > 0 ? ")" : "", system_text);
    pthread_mutex_unlock(&console_dns_lock);
}

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **result)
{
    *result = NULL;
    const int family = hints != NULL ? hints->ai_family : AF_UNSPEC;
    if (family != AF_UNSPEC && family != AF_INET)
        return EAI_FAMILY;

    uint32_t addresses[CONSOLE_DNS_MAX];
    int count = 1;
    addresses[0] = htonl(INADDR_LOOPBACK);
    if (node != NULL)
    {
        if (hints != NULL && (hints->ai_flags & AI_NUMERICHOST) != 0)
        {
            struct in_addr numeric;
            if (inet_pton(AF_INET, node, &numeric) != 1)
                return EAI_NONAME;
            addresses[0] = numeric.s_addr;
        }
        else
        {
            const int failed = console_curl_lookup(node, addresses, &count);
            if (failed != 0)
                return failed;
        }
    }
    else if (hints != NULL && (hints->ai_flags & AI_PASSIVE) != 0)
    {
        addresses[0] = htonl(INADDR_ANY);
    }

    /* One entry per address, so curl moves on when one refuses to connect.
     * Each entry and its address are one allocation; freeaddrinfo frees each. */
    struct addrinfo *head = NULL, **tail = &head;
    for (int i = 0; i < count; ++i)
    {
        struct addrinfo *entry = calloc(1, sizeof(struct addrinfo) + sizeof(struct sockaddr_in));
        if (entry == NULL)
        {
            freeaddrinfo(head);
            return EAI_MEMORY;
        }
        struct sockaddr_in *in = (struct sockaddr_in *)(entry + 1);
        in->sin_len = sizeof(*in);
        in->sin_family = AF_INET;
        in->sin_port = htons((uint16_t)(service != NULL ? atoi(service) : 0));
        in->sin_addr.s_addr = addresses[i];
        entry->ai_family = AF_INET;
        entry->ai_socktype =
            hints != NULL && hints->ai_socktype != 0 ? hints->ai_socktype : SOCK_STREAM;
        entry->ai_protocol = hints != NULL ? hints->ai_protocol : 0;
        entry->ai_addrlen = sizeof(*in);
        entry->ai_addr = (struct sockaddr *)in;
        *tail = entry;
        tail = &entry->ai_next;
    }
    *result = head;
    return 0;
}

void freeaddrinfo(struct addrinfo *entry)
{
    while (entry != NULL)
    {
        struct addrinfo *next = entry->ai_next;
        free(entry);
        entry = next;
    }
}

const char *gai_strerror(int code)
{
    switch (code)
    {
    case 0:
        return "no error";
    case EAI_NONAME:
        return "the name was not found";
    case EAI_FAMILY:
        return "address family not supported";
    case EAI_MEMORY:
        return "out of memory";
    default:
        return "the name lookup failed";
    }
}

struct hostent *gethostbyname(const char *name)
{
    (void)name;
    return NULL; /* libcurl uses getaddrinfo */
}

/* Numeric only: enough for libcurl's record of the connected address. */
int getnameinfo(const struct sockaddr *address, socklen_t length, char *host, size_t host_size,
                char *service, size_t service_size, int flags)
{
    (void)flags;
    if (address == NULL || address->sa_family != AF_INET || length < sizeof(struct sockaddr_in))
        return EAI_FAMILY;
    const struct sockaddr_in *in = (const struct sockaddr_in *)address;
    if (host != NULL && host_size != 0 &&
        inet_ntop(AF_INET, &in->sin_addr, host, (socklen_t)host_size) == NULL)
        return EAI_FAIL;
    if (service != NULL && service_size != 0)
        snprintf(service, service_size, "%u", (unsigned)ntohs(in->sin_port));
    return 0;
}

/* '*' and '?' only: all libcurl's wildcard matching asks for. */
int fnmatch(const char *pattern, const char *text, int flags)
{
    for (; *pattern != '\0'; ++pattern, ++text)
    {
        if (*pattern == '*')
        {
            while (*pattern == '*')
                ++pattern;
            if (*pattern == '\0')
                return 0;
            for (; *text != '\0'; ++text)
            {
                if (fnmatch(pattern, text, flags) == 0)
                    return 0;
            }
            return FNM_NOMATCH;
        }
        if (*text == '\0' || (*pattern != '?' && *pattern != *text))
            return FNM_NOMATCH;
    }
    return *text == '\0' ? 0 : FNM_NOMATCH;
}

/* ---- libc stand-ins ------------------------------------------------------------------------ */

/* No user database: "no such user", so curl looks for no home folder. */
struct passwd;
int getpwuid_r(unsigned user, struct passwd *entry, char *buffer, size_t size,
               struct passwd **result)
{
    (void)user;
    (void)entry;
    (void)buffer;
    (void)size;
    *result = NULL;
    return ENOENT;
}

/* The archives call _setjmp/_longjmp; the console exports setjmp/longjmp. A
 * plain jump keeps the caller's frame, which setjmp needs. x86-64 only. */
__asm__(".text\n"
        ".globl _setjmp\n"
        "_setjmp:\n"
        "    jmp *setjmp@GOTPCREL(%rip)\n"
        ".globl _longjmp\n"
        "_longjmp:\n"
        "    jmp *longjmp@GOTPCREL(%rip)\n");

/* openlog/closelog, popen/pclose, isatty and mkstemp live in
 * src/runtime/runtime_shims.c, which every build links; console_curl.c only
 * adds what libcurl itself needs beyond them. */

void closelog(void)
{
}

int dladdr(const void *address, void *info)
{
    (void)address;
    (void)info;
    return 0;
}

unsigned if_nametoindex(const char *name)
{
    (void)name;
    return 0;
}

int pipe2(int descriptors[2], int flags)
{
    if (pipe(descriptors) != 0)
        return -1;
    for (int i = 0; i < 2; ++i)
    {
        if ((flags & O_NONBLOCK) != 0)
            (void)fcntl(descriptors[i], F_SETFL, fcntl(descriptors[i], F_GETFL) | O_NONBLOCK);
        if ((flags & O_CLOEXEC) != 0)
            (void)fcntl(descriptors[i], F_SETFD, FD_CLOEXEC);
    }
    return 0;
}

/* HTTP/3 only; curl falls back. */
ssize_t recvmmsg(int socket, struct mmsghdr *restrict messages, size_t count, int flags,
                 const struct timespec *restrict timeout)
{
    (void)socket;
    (void)messages;
    (void)count;
    (void)flags;
    (void)timeout;
    errno = ENOSYS;
    return -1;
}

ssize_t sendmmsg(int socket, struct mmsghdr *restrict messages, size_t count, int flags)
{
    (void)socket;
    (void)messages;
    (void)count;
    (void)flags;
    errno = ENOSYS;
    return -1;
}

/* zstd's optional tracing hooks; 0 means "not traced". */
unsigned long long ZSTD_trace_compress_begin(const void *context)
{
    (void)context;
    return 0;
}

void ZSTD_trace_compress_end(unsigned long long trace, const void *record)
{
    (void)trace;
    (void)record;
}

unsigned long long ZSTD_trace_decompress_begin(const void *context)
{
    (void)context;
    return 0;
}

void ZSTD_trace_decompress_end(unsigned long long trace, const void *record)
{
    (void)trace;
    (void)record;
}

/* OpenSSL checks certificate dates with it, so it must be right. Days since
 * 1970 to a civil date (Howard Hinnant's algorithm). */
struct tm *gmtime_r(const time_t *when, struct tm *out)
{
    long long seconds = (long long)*when;
    long long days = seconds / 86400;
    long long rest = seconds % 86400;
    if (rest < 0)
    {
        rest += 86400;
        --days;
    }
    memset(out, 0, sizeof(*out));
    out->tm_hour = (int)(rest / 3600);
    out->tm_min = (int)(rest % 3600 / 60);
    out->tm_sec = (int)(rest % 60);
    out->tm_wday = (int)(((days % 7) + 11) % 7);
    const long long z = days + 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const long long doe = z - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    const long long month = mp < 10 ? mp + 3 : mp - 9;
    const long long year = yoe + era * 400 + (month <= 2 ? 1 : 0);
    out->tm_mday = (int)(doy - (153 * mp + 2) / 5 + 1);
    out->tm_mon = (int)(month - 1);
    out->tm_year = (int)(year - 1900);
    const int leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    static const int before[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    out->tm_yday = before[out->tm_mon] + out->tm_mday - 1 + (leap && out->tm_mon > 1 ? 1 : 0);
    return out;
}

/* libavutil and libavformat print dates: the console has no timezone
 * database, so local time is UTC. */
struct tm *localtime_r(const time_t *when, struct tm *out)
{
    return gmtime_r(when, out);
}

/* libiconv/libcharset probe the locale; everything this app decodes is
 * UTF-8, and that is also the honest answer for text on the wire. */
char *nl_langinfo(nl_item item)
{
    (void)item;
    return (char *)"UTF-8";
}

/* The SDK header declares this as a function (MB_CUR_MAX calls it); the
 * console's libc does not export it, libiconv needs it. */
int ___mb_cur_max(void)
{
    return 4; /* UTF-8 */
}
