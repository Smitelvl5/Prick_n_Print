#ifndef BACKEND_TLS_H
#define BACKEND_TLS_H

#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <mbedtls/x509_crt.h>
#include "Logger.h"

// Root CAs for https:// backends (PEM bundle). Tailscale Funnel (*.ts.net) serves Let's Encrypt
// certs. Its ECDSA chain is leaf <- YE1 <- Root YE <- ISRG Root X2 <- (cross-sign) ISRG Root X1.
// The TZT's mbedTLS rejected that chain when it had to go all the way up to X1 ("X509 - Certificate
// verification failed", seen 2026-11-11, while OpenSSL accepted it), so X2 is trusted directly too:
// verification then ends at Root YE <- X2 (ECDSA P-384). X1 stays for the RSA chain (YR <- Root YR <- X1).
// ISRG Root X1: valid until 2035-06-04, SHA-256 96:BC:EC:06:26:49:76:F3:...:BD:DF:08:C6
// ISRG Root X2: valid until 2040-09-17, SHA-256 69:72:9B:8E:15:A8:6E:FC:...:3C:CB:14:70
static const char BACKEND_ROOT_CAS_PEM[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIFazCCA1OgAwIBAgIRAIIQz7DSQONZRGPgu2OCiwAwDQYJKoZIhvcNAQELBQAw\n"
"TzELMAkGA1UEBhMCVVMxKTAnBgNVBAoTIEludGVybmV0IFNlY3VyaXR5IFJlc2Vh\n"
"cmNoIEdyb3VwMRUwEwYDVQQDEwxJU1JHIFJvb3QgWDEwHhcNMTUwNjA0MTEwNDM4\n"
"WhcNMzUwNjA0MTEwNDM4WjBPMQswCQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJu\n"
"ZXQgU2VjdXJpdHkgUmVzZWFyY2ggR3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBY\n"
"MTCCAiIwDQYJKoZIhvcNAQEBBQADggIPADCCAgoCggIBAK3oJHP0FDfzm54rVygc\n"
"h77ct984kIxuPOZXoHj3dcKi/vVqbvYATyjb3miGbESTtrFj/RQSa78f0uoxmyF+\n"
"0TM8ukj13Xnfs7j/EvEhmkvBioZxaUpmZmyPfjxwv60pIgbz5MDmgK7iS4+3mX6U\n"
"A5/TR5d8mUgjU+g4rk8Kb4Mu0UlXjIB0ttov0DiNewNwIRt18jA8+o+u3dpjq+sW\n"
"T8KOEUt+zwvo/7V3LvSye0rgTBIlDHCNAymg4VMk7BPZ7hm/ELNKjD+Jo2FR3qyH\n"
"B5T0Y3HsLuJvW5iB4YlcNHlsdu87kGJ55tukmi8mxdAQ4Q7e2RCOFvu396j3x+UC\n"
"B5iPNgiV5+I3lg02dZ77DnKxHZu8A/lJBdiB3QW0KtZB6awBdpUKD9jf1b0SHzUv\n"
"KBds0pjBqAlkd25HN7rOrFleaJ1/ctaJxQZBKT5ZPt0m9STJEadao0xAH0ahmbWn\n"
"OlFuhjuefXKnEgV4We0+UXgVCwOPjdAvBbI+e0ocS3MFEvzG6uBQE3xDk3SzynTn\n"
"jh8BCNAw1FtxNrQHusEwMFxIt4I7mKZ9YIqioymCzLq9gwQbooMDQaHWBfEbwrbw\n"
"qHyGO0aoSCqI3Haadr8faqU9GY/rOPNk3sgrDQoo//fb4hVC1CLQJ13hef4Y53CI\n"
"rU7m2Ys6xt0nUW7/vGT1M0NPAgMBAAGjQjBAMA4GA1UdDwEB/wQEAwIBBjAPBgNV\n"
"HRMBAf8EBTADAQH/MB0GA1UdDgQWBBR5tFnme7bl5AFzgAiIyBpY9umbbjANBgkq\n"
"hkiG9w0BAQsFAAOCAgEAVR9YqbyyqFDQDLHYGmkgJykIrGF1XIpu+ILlaS/V9lZL\n"
"ubhzEFnTIZd+50xx+7LSYK05qAvqFyFWhfFQDlnrzuBZ6brJFe+GnY+EgPbk6ZGQ\n"
"3BebYhtF8GaV0nxvwuo77x/Py9auJ/GpsMiu/X1+mvoiBOv/2X/qkSsisRcOj/KK\n"
"NFtY2PwByVS5uCbMiogziUwthDyC3+6WVwW6LLv3xLfHTjuCvjHIInNzktHCgKQ5\n"
"ORAzI4JMPJ+GslWYHb4phowim57iaztXOoJwTdwJx4nLCgdNbOhdjsnvzqvHu7Ur\n"
"TkXWStAmzOVyyghqpZXjFaH3pO3JLF+l+/+sKAIuvtd7u+Nxe5AW0wdeRlN8NwdC\n"
"jNPElpzVmbUq4JUagEiuTDkHzsxHpFKVK7q4+63SM1N95R1NbdWhscdCb+ZAJzVc\n"
"oyi3B43njTOQ5yOf+1CceWxG1bQVs5ZufpsMljq4Ui0/1lvh+wjChP4kqKOJ2qxq\n"
"4RgqsahDYVvTH9w7jXbyLeiNdd8XM2w9U/t7y0Ff/9yi0GE44Za4rF2LN9d11TPA\n"
"mRGunUHBcnWEvgJBQl9nJEiU0Zsnvgc/ubhPgXRR4Xq37Z0j4r7g1SgEEzwxA57d\n"
"emyPxgcYxn/eR44/KJ4EBs+lVDR3veyJm+kXQ99b21/+jh5Xos1AnX5iItreGCc=\n"
"-----END CERTIFICATE-----\n"
"-----BEGIN CERTIFICATE-----\n"
"MIICGzCCAaGgAwIBAgIQQdKd0XLq7qeAwSxs6S+HUjAKBggqhkjOPQQDAzBPMQsw\n"
"CQYDVQQGEwJVUzEpMCcGA1UEChMgSW50ZXJuZXQgU2VjdXJpdHkgUmVzZWFyY2gg\n"
"R3JvdXAxFTATBgNVBAMTDElTUkcgUm9vdCBYMjAeFw0yMDA5MDQwMDAwMDBaFw00\n"
"MDA5MTcxNjAwMDBaME8xCzAJBgNVBAYTAlVTMSkwJwYDVQQKEyBJbnRlcm5ldCBT\n"
"ZWN1cml0eSBSZXNlYXJjaCBHcm91cDEVMBMGA1UEAxMMSVNSRyBSb290IFgyMHYw\n"
"EAYHKoZIzj0CAQYFK4EEACIDYgAEzZvVn4CDCuwJSvMWSj5cz3es3mcFDR0HttwW\n"
"+1qLFNvicWDEukWVEYmO6gbf9yoWHKS5xcUy4APgHoIYOIvXRdgKam7mAHf7AlF9\n"
"ItgKbppbd9/w+kHsOdx1ymgHDB/qo0IwQDAOBgNVHQ8BAf8EBAMCAQYwDwYDVR0T\n"
"AQH/BAUwAwEB/zAdBgNVHQ4EFgQUfEKWrt5LSDv6kviejM9ti6lyN5UwCgYIKoZI\n"
"zj0EAwMDaAAwZQIwe3lORlCEwkSHRhtFcP9Ymd70/aTSVaYgLXTWNLxBo1BfASdW\n"
"tL4ndQavEi51mI38AjEAi/V3bNTIZargCyzuFJ0nN6T5U6VR5CmD1/iQMVtCnwr1\n"
"/q4AaOeMSQ+2b1tbFfLn\n"
"-----END CERTIFICATE-----\n";

// TLS client that checks the server's certificate chain right after the handshake instead of
// during it. Same roots, same checks (chain + hostname), and nothing is sent before it passes.
// Why: on the ESP32 the in-handshake check of this chain (all ecdsa-with-SHA384) fails now and then
// with "X509 - Certificate verification failed" while the identical check after the handshake passes:
// during the handshake a SHA-384 transcript hash holds the SHA hardware, and the certificate hashes
// computed meanwhile come out wrong. Seen 2026-11-11 on the TZT, which then couldn't reach the Pi.
// The handshake itself still proves the server holds the certificate's key.
class VerifiedTlsClient : public WiFiClientSecure {
public:
    VerifiedTlsClient() { setInsecure(); }  // the chain is checked in connect() below

    int connect(const char* host, uint16_t port) override {
        verifyError = "";
        if (!WiFiClientSecure::connect(host, port)) return 0;
        if (verifyPeer(host)) return 1;
        stop();
        return 0;
    }

    // Why the last connect() was refused after the handshake ("" = it wasn't)
    String verifyError;
    int verifyTries = 0;  // attempts the last chain check took (1 = first time)

private:
    bool verifyPeer(const char* host) {
        const mbedtls_x509_crt* chain = sslclient ? mbedtls_ssl_get_peer_cert(&sslclient->ssl_ctx) : nullptr;
        if (!chain) {
            verifyError = "no server certificate";
            return false;
        }
        mbedtls_x509_crt roots;
        mbedtls_x509_crt_init(&roots);
        int ret = mbedtls_x509_crt_parse(&roots, (const unsigned char*)BACKEND_ROOT_CAS_PEM,
                                         strlen(BACKEND_ROOT_CAS_PEM) + 1);
        // The check is deterministic, yet on the TZT it has come out "not correctly signed" now and
        // then for this valid chain (crypto hardware glitch): retry a few times before refusing
        uint32_t flags = 0;
        verifyTries = 0;
        if (ret == 0) {
            do {
                flags = 0;
                verifyTries++;
                ret = mbedtls_x509_crt_verify((mbedtls_x509_crt*)chain, &roots, nullptr, host, &flags, nullptr, nullptr);
            } while (ret != 0 && verifyTries < 5);
        }
        mbedtls_x509_crt_free(&roots);
        if (ret == 0) {
            if (verifyTries > 1) Logger::warn("TLS", "Certificate check passed on try " + String(verifyTries));
            return true;
        }
        char info[160];
        mbedtls_x509_crt_verify_info(info, sizeof(info), "", flags);
        verifyError = "certificate rejected after " + String(verifyTries) + " tries (" + String(ret) + ", flags 0x" + String(flags, HEX) + ") " + info;
        verifyError.replace("\n", "; ");
        return false;
    }
};

// http.begin() that verifies the server cert for https:// URLs (plain http:// unchanged)
inline bool beginBackendHttp(HTTPClient& http, const String& url) {
    if (url.startsWith("https://")) return http.begin(url, BACKEND_ROOT_CAS_PEM);
    return http.begin(url);
}

#endif // BACKEND_TLS_H
