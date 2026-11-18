#pragma once
// Existing synthetic TLS fixtures, shared by network tests; protect no real data.
namespace PlankTlsFixture {
inline const char EcCertificateBase64[] =
    "MIIBtDCCAVmgAwIBAgIUCRdNSSSN3QeyedMR/2NNJjI1CRowCgYIKoZIzj0EAwIwHjEcMBoGA1UE"
    "AwwTYnJva2VyLmV4YW1wbGUudGVzdDAgFw0yNjA5MjExNDA5NTRaGA8yMTI2MDgyODE0MDk1NFow"
    "HjEcMBoGA1UEAwwTYnJva2VyLmV4YW1wbGUudGVzdDBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IA"
    "BILYjWo0puuGeIwCH04JTdCwlrSOeTNC3g5S7fRp9AEGiT1gtZ8dGcrGOifhmaMrgnQA8NBiABzr"
    "8VlkuGwcOrajczBxMB0GA1UdDgQWBBTOHP6zpycxw0Iuu9Qb8VWEN3lVjzAfBgNVHSMEGDAWgBTO"
    "HP6zpycxw0Iuu9Qb8VWEN3lVjzAPBgNVHRMBAf8EBTADAQH/MB4GA1UdEQQXMBWCE2Jyb2tlci5l"
    "eGFtcGxlLnRlc3QwCgYIKoZIzj0EAwIDSQAwRgIhAKJtlaaULDAPMm3HniayaWPdtlOPUXKwWvAU"
    "US8KGpB6AiEAnekyGhNtj05Lfadv9uVX8C8UOhoH5H/ztN2hNc+ui0g=";
inline const char EcSpkiSha256[] = "91bb7956f2378b1ed82e7f52b18d5ce8ca2a4f6d835919872083498f517b09c8";
inline const char EcTestOnlyPrivateKeyPem[] =
    "-----BEGIN PRIVATE KEY-----\n"
    "MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgczd2sUy0hEU5M5o/\n"
    "XskQT9pkYDROwzubJkO4EjHBmE+hRANCAASC2I1qNKbrhniMAh9OCU3QsJa0jnkz\n"
    "Qt4OUu30afQBBok9YLWfHRnKxjon4ZmjK4J0APDQYgAc6/FZZLhsHDq2\n"
    "-----END PRIVATE KEY-----\n";
}
