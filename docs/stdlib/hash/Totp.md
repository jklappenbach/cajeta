# Totp

`cajeta.hash.Totp` — one-time passwords: HOTP (RFC 4226) and the time-based
TOTP (RFC 6238) over [HmacSha1](HmacSha1.md), as Google Authenticator,
Microsoft Authenticator and the rest compute them. Six digits and a
thirty-second step are the values those apps use whatever the enrollment URI
says, so `uri` writes exactly those. Verification policy, the window either
side of now, single use per step and attempt lockout, is the caller's.

```cajeta
int8[] secret #= SecureRandom.bytes(20);
String enroll #= Totp.uri("Example", "ada@example.test", secret, 20);   // render as a QR code
int64 unixSeconds = 59L;
int32 now = Totp.code(secret, 20, unixSeconds, 30, 6);
String shown #= Totp.format(now, 6);                                    // "007081", zero padded
```

## Methods

| Signature | |
|---|---|
| `static int32 hotp(int8[] secret, int64 secretLen, int64 counter, int32 digits)` | RFC 4226 code for a counter |
| `static int32 code(int8[] secret, int64 secretLen, int64 unixSeconds, int32 stepSeconds, int32 digits)` | RFC 6238 code for the step containing `unixSeconds` |
| `static #String format(int32 code, int32 digits)` | The code as `digits` characters, zero padded |
| `static #String uri(String issuer, String account, int8[] secret, int64 secretLen)` | The `otpauth://totp/` enrollment URI: Base32 secret, SHA1, 6 digits, period 30, issuer and account percent-encoded |

## See also

- Source: [`runtime/src/cajeta/hash/Totp.cajeta`](../../../runtime/src/cajeta/hash/Totp.cajeta)
- [HmacSha1](HmacSha1.md) — the MAC underneath, [Base32](../codec/Base32.md) — how the secret travels, [SecureRandom](SecureRandom.md) — where it comes from
