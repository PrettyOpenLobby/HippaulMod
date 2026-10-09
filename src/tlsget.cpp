// tlsget.cpp -- HTTPS for the legacy XP build, over BearSSL and plain sockets.
//
// WHY THE XP BUILD CARRIES ITS OWN TLS
//
// The release feed is GitHub, and GitHub accepts TLS 1.2 and later only. XP's
// SChannel, which WinINet uses, stops at TLS 1.0 and sends no SNI, so every
// HTTPS request from an XP install fails before a byte of HTTP is exchanged.
// BearSSL is small, needs nothing from the OS beyond sockets, CryptGenRandom and
// the clock, and picks its SSE2 / AES-NI code paths by CPUID at run time, so it
// also runs on a CPU without SSE2.
//
// The trusted roots are compiled in (tlsroots.c). XP's own certificate store is
// not consulted: an unpatched XP machine has neither of the roots GitHub's
// chains end at today.
//
// Plain http:// stays on WinINet in both builds; only https:// comes here.
#include "polshim.h"
#include <winsock2.h>
#include <ws2tcpip.h>
#include "tlsget.h"
extern "C" {
#include "bearssl.h"
const br_x509_trust_anchor* tls_roots(size_t* n);
}

static const DWORD CONNECT_MS = 6000;
static const DWORD IO_MS      = 15000;

struct TlsConn {
    br_ssl_client_context   sc;
    br_x509_minimal_context xc;
    br_sslio_context        io;
    unsigned char           iobuf[BR_SSL_BUFSIZE_BIDI];
    SOCKET                  s;
};

static int sock_read(void* ctx, unsigned char* buf, size_t len)
{
    int r = recv(*(SOCKET*)ctx, (char*)buf, (int)len, 0);
    return r > 0 ? r : -1;
}

static int sock_write(void* ctx, const unsigned char* buf, size_t len)
{
    int r = send(*(SOCKET*)ctx, (const char*)buf, (int)len, 0);
    return r > 0 ? r : -1;
}

static void say(char* err, size_t cap, const char* fmt, ...)
{
    if (!err || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(err, cap, _TRUNCATE, fmt, ap);
    va_end(ap);
}

// The BearSSL codes a player is likely to hit, in words. The clock one matters
// most: an old PC with a flat CMOS battery boots in 2002, and every certificate
// is then "not yet valid".
static const char* tls_reason(int e)
{
    switch (e) {
    case BR_ERR_X509_EXPIRED:     return "certificate expired or not yet valid (is this PC's clock right?)";
    case BR_ERR_X509_NOT_TRUSTED: return "certificate not signed by a root this build trusts";
    case BR_ERR_X509_BAD_SERVER_NAME: return "certificate is for a different host";
    case BR_ERR_BAD_VERSION:      return "server refused every TLS version offered";
    case BR_ERR_IO:               return "connection dropped";
    default:                      return "TLS error";
    }
}

static bool parse_url(const char* url, char* host, size_t hcap, char* port, size_t pcap,
                      char* path, size_t pathcap)
{
    if (_strnicmp(url, "https://", 8) != 0) return false;
    const char* h = url + 8;
    const char* end = h + strcspn(h, "/?#");
    const char* colon = (const char*)memchr(h, ':', end - h);
    const char* hend = colon ? colon : end;
    if (hend == h || (size_t)(hend - h) >= hcap) return false;
    memcpy(host, h, hend - h); host[hend - h] = 0;
    if (colon) {
        size_t n = end - colon - 1;
        if (!n || n >= pcap) return false;
        memcpy(port, colon + 1, n); port[n] = 0;
    } else {
        strcpy_s(port, pcap, "443");
    }
    _snprintf_s(path, pathcap, _TRUNCATE, "%s%s", *end == '/' ? "" : "/", end);
    return true;
}

static SOCKET connect_to(const char* host, const char* port, char* err, size_t ecap)
{
    addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0 || !res) {
        say(err, ecap, "could not look up %s", host);
        return INVALID_SOCKET;
    }
    SOCKET s = INVALID_SOCKET;
    for (addrinfo* a = res; a && s == INVALID_SOCKET; a = a->ai_next) {
        s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        // Non-blocking connect, so an unreachable host costs CONNECT_MS and not
        // the OS's own twenty-odd seconds.
        u_long nb = 1;
        ioctlsocket(s, FIONBIO, &nb);
        bool up = connect(s, a->ai_addr, (int)a->ai_addrlen) == 0;
        if (!up && WSAGetLastError() == WSAEWOULDBLOCK) {
            fd_set w, x;
            FD_ZERO(&w); FD_SET(s, &w);
            FD_ZERO(&x); FD_SET(s, &x);
            timeval tv = { (long)(CONNECT_MS / 1000), (long)(CONNECT_MS % 1000) * 1000 };
            if (select(0, NULL, &w, &x, &tv) == 1 && FD_ISSET(s, &w)) {
                int soerr = 0, sl = sizeof(soerr);
                up = getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soerr, &sl) == 0 && soerr == 0;
            }
        }
        if (!up) { closesocket(s); s = INVALID_SOCKET; continue; }
        nb = 0;
        ioctlsocket(s, FIONBIO, &nb);
        DWORD t = IO_MS;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&t, sizeof(t));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&t, sizeof(t));
    }
    freeaddrinfo(res);
    if (s == INVALID_SOCKET) say(err, ecap, "could not connect to %s:%s", host, port);
    return s;
}

// Header value by name (case-insensitive), copied into out. hdr is the block
// between the status line and the blank line, NUL-terminated.
static bool header(const char* hdr, const char* name, char* out, size_t cap)
{
    size_t nl = strlen(name);
    for (const char* p = hdr; *p; ) {
        const char* eol = strstr(p, "\r\n");
        if (!eol) eol = p + strlen(p);
        if ((size_t)(eol - p) > nl && _strnicmp(p, name, nl) == 0 && p[nl] == ':') {
            const char* v = p + nl + 1;
            while (v < eol && (*v == ' ' || *v == '\t')) v++;
            size_t n = eol - v;
            if (n >= cap) n = cap - 1;
            memcpy(out, v, n); out[n] = 0;
            return true;
        }
        p = *eol ? eol + 2 : eol;
    }
    return false;
}

// Decode a chunked body in place. False when the final zero-length chunk never
// arrived, which means the body was cut short.
static bool unchunk(BYTE* b, DWORD* len)
{
    DWORD in = 0, out = 0;
    for (;;) {
        DWORD n = 0; bool any = false;
        while (in < *len && isxdigit(b[in])) {
            int c = b[in++];
            n = n * 16 + (DWORD)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
            any = true;
            if (n > 0x10000000u) return false;
        }
        while (in < *len && b[in] != '\n') in++;          // chunk extensions, CR
        if (!any || in >= *len) return false;
        in++;
        if (n == 0) { *len = out; return true; }
        if (n > *len - in) return false;
        memmove(b + out, b + in, n);
        out += n; in += n;
        if (in + 2 > *len || b[in] != '\r' || b[in + 1] != '\n') return false;
        in += 2;
    }
}

// One request. Returns the HTTP status (0 on a transport failure), with the
// body for a 200 or the Location for a redirect.
static int fetch_once(const char* url, const char* agent, DWORD cap, BYTE** body,
                      DWORD* blen, char* loc, size_t loccap, char* err, size_t ecap)
{
    char host[256], port[8], path[1024];
    if (!parse_url(url, host, sizeof(host), port, sizeof(port), path, sizeof(path))) {
        say(err, ecap, "not an https URL");
        return 0;
    }
    TlsConn* c = (TlsConn*)calloc(1, sizeof(TlsConn));
    if (!c) { say(err, ecap, "out of memory"); return 0; }
    c->s = connect_to(host, port, err, ecap);
    if (c->s == INVALID_SOCKET) { free(c); return 0; }

    size_t nroots = 0;
    const br_x509_trust_anchor* roots = tls_roots(&nroots);
    br_ssl_client_init_full(&c->sc, &c->xc, roots, nroots);
    br_ssl_engine_set_buffer(&c->sc.eng, c->iobuf, sizeof(c->iobuf), 1);
    br_ssl_client_reset(&c->sc, host, 0);
    br_sslio_init(&c->io, &c->sc.eng, sock_read, &c->s, sock_write, &c->s);

    char req[1536];
    int rl = _snprintf_s(req, sizeof(req), _TRUNCATE,
                         "GET %s HTTP/1.1\r\nHost: %s%s%s\r\nUser-Agent: %s\r\n"
                         "Accept: */*\r\nConnection: close\r\n\r\n",
                         path, host, strcmp(port, "443") ? ":" : "",
                         strcmp(port, "443") ? port : "", agent);
    int status = 0;
    BYTE* buf = NULL;
    DWORD used = 0, size = 0;
    const DWORD limit = cap + 64 * 1024;                 // the body plus headers
    if (rl < 0 || br_sslio_write_all(&c->io, req, rl) < 0 || br_sslio_flush(&c->io) < 0) {
        int e = br_ssl_engine_last_error(&c->sc.eng);
        say(err, ecap, "%s: %s (%d)", host, tls_reason(e), e);
        goto done;
    }
    for (;;) {
        if (used == size) {
            if (size >= limit) { say(err, ecap, "%s: response larger than %lu bytes", host, cap); goto done; }
            size = size ? size * 2 : 64 * 1024;
            if (size > limit) size = limit;
            BYTE* nb = (BYTE*)realloc(buf, size + 1);
            if (!nb) { say(err, ecap, "out of memory"); goto done; }
            buf = nb;
        }
        int r = br_sslio_read(&c->io, buf + used, size - used);
        if (r < 0) break;
        used += (DWORD)r;
    }
    {
        int e = br_ssl_engine_last_error(&c->sc.eng);
        if (!used) { say(err, ecap, "%s: %s (%d)", host, tls_reason(e), e); goto done; }
        buf[used] = 0;
        BYTE* hend = (BYTE*)strstr((char*)buf, "\r\n\r\n");
        if (!hend || strncmp((char*)buf, "HTTP/1.", 7) != 0 || used < 12) {
            say(err, ecap, "%s: not an HTTP response", host);
            goto done;
        }
        status = atoi((char*)buf + 9);
        *hend = 0;
        const char* hdr = strstr((char*)buf, "\r\n");
        hdr = hdr ? hdr + 2 : "";
        if (status >= 300 && status < 400) {
            if (!header(hdr, "Location", loc, loccap)) {
                say(err, ecap, "%s: redirect without a Location", host);
                status = 0;
            }
            goto done;
        }
        if (status != 200) goto done;

        BYTE* b = hend + 4;
        DWORD n = used - (DWORD)(b - buf);
        char v[64];
        // Completeness. The DLL itself is hash-checked by the caller, but the
        // .sha256 beside it is not, so a short read must not pass as a body.
        if (header(hdr, "Transfer-Encoding", v, sizeof(v)) && strstr(v, "chunked")) {
            if (!unchunk(b, &n)) { say(err, ecap, "%s: chunked body cut short", host); status = 0; goto done; }
        } else if (header(hdr, "Content-Length", v, sizeof(v))) {
            DWORD want = (DWORD)strtoul(v, NULL, 10);
            if (n < want) { say(err, ecap, "%s: body cut short (%lu of %lu bytes)", host, n, want); status = 0; goto done; }
            n = want;
        }
        if (n > cap) { say(err, ecap, "%s: body larger than %lu bytes", host, cap); status = 0; goto done; }
        BYTE* out = (BYTE*)malloc(n ? n : 1);
        if (!out) { say(err, ecap, "out of memory"); status = 0; goto done; }
        memcpy(out, b, n);
        *body = out;
        *blen = n;
    }
done:
    free(buf);
    closesocket(c->s);
    free(c);
    return status;
}

BYTE* tls_https_get(const char* url, DWORD* out_len, DWORD cap, const char* agent,
                    char* err, size_t errcap)
{
    if (err && errcap) err[0] = 0;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) { say(err, errcap, "winsock unavailable"); return NULL; }
    char cur[1024];
    strcpy_s(cur, sizeof(cur), url);
    BYTE* result = NULL;
    for (int hop = 0; hop <= 5; hop++) {
        BYTE* body = NULL; DWORD blen = 0;
        char loc[1024] = "";
        int st = fetch_once(cur, agent, cap, &body, &blen, loc, sizeof(loc), err, errcap);
        if (st == 200) { result = body; *out_len = blen; break; }
        if (st >= 300 && st < 400 && loc[0]) {
            if (_strnicmp(loc, "https://", 8) == 0) {
                strcpy_s(cur, sizeof(cur), loc);
            } else if (loc[0] == '/') {
                // Same host, new path: keep "https://host[:port]".
                char* p = strchr(cur + 8, '/');
                if (p) *p = 0;
                strncat_s(cur, sizeof(cur), loc, _TRUNCATE);
            } else {
                // http:// would be a downgrade; anything else is not a URL we follow.
                say(err, errcap, "refusing redirect to %.80s", loc);
                break;
            }
            continue;
        }
        if (st) say(err, errcap, "HTTP %d", st);
        break;
    }
    if (!result && err && !err[0]) say(err, errcap, "too many redirects");
    WSACleanup();
    return result;
}
