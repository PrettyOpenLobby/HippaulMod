// tlsget.h -- HTTPS GET for the legacy XP build (tlsget.cpp). Not built otherwise.
#pragma once
#ifdef POLSHIM_XP

// GET an https:// URL, following up to five redirects, and return the body of
// the final 200 as a malloc'd buffer (the caller frees it), or NULL with a
// one-line reason in err. A body larger than cap is refused.
BYTE* tls_https_get(const char* url, DWORD* out_len, DWORD cap, const char* agent,
                    char* err, size_t errcap);

#endif
