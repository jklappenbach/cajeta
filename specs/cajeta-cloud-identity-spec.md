# cajeta-cloud identity — the end-user identity port

Addendum to `cajeta-cloud-spec.md`. Adds one service family, **identity**,
under the parent's rules: a port, an in-memory driver, and a conformance
testkit in `cajeta-cloud`, every real backend in a provider artifact
(`cajeta-cloud-aws`, `cajeta-cloud-azure`, `cajeta-cloud-gcp`,
`cajeta-cloud-cloudflare`). Section references of the form *parent §n* point
at the parent spec.

## 1. Definition

### 1.1 Purpose

Every application with users needs the same five things from an identity
service: register an account, prove who is signing in, run a second-factor
challenge, hand back tokens the rest of the system can verify, and expose the
attributes and group memberships the application authorizes on. The managed
providers converged on this shape: Amazon Cognito user pools, Microsoft Entra
External ID, Google Cloud Identity Platform, and Cloudflare Access all offer
it. The port names that shape once, so `primavera-security` and any other
consumer is written against `cajeta-cloud` alone (parent §2.1) and the
provider is configuration (parent §2.2).

The first consumer is the primavera user-registration sample
(`primavera-web-spec.md` §5). That sample is why the port starts minimal:
registration and lookup ship first, and authentication, challenges and tokens
follow when primavera's security phase needs them (§1.6).

### 1.2 What this addendum changes in the parent

- Parent §1.2 says "Identity is cross-cutting, not a row" and points at §9.5.
  That sentence is about an **adapter's own credentials**: signing keys, role
  assumption. It stays true for that. **End-user identity is a different
  thing and is a row.** The family table gains an `identity` row and the
  status table marks it specified here.
- Nothing else in the parent changes. The three-implementation rule (parent
  §1.4), capability negotiation (§3), adapters elsewhere (§1.7), the testkit
  (§10) and the error taxonomy (§11) all apply to this port unchanged.

### 1.3 The reference implementation is in memory, and only in memory

The in-memory driver is the reference implementation and the default when no
provider adapter is compiled in (§9). It holds users, credentials, challenge
state and issued tokens in process, and it is faithful to the contract
including its failure modes: a duplicate registration conflicts, a wrong
password fails, an expired code is rejected.

A filesystem-backed driver does **not** live in `cajeta-cloud`. Parent §1.4
keeps the library at `capabilities: []`, and parent §1.5 places every local
backend in a separate, uncommitted `cajeta-cloud-local`. If the registration
sample needs users to survive a restart before a provider adapter exists, the
filesystem driver is written in `cajeta-cloud-local` and the sample depends on
that artifact. The sample never carries a user store of its own.

### 1.4 Scope

The identity port: registration, lookup, authentication, challenges,
tokens, attributes and groups (§2 to §8). Capability declarations for the
features providers disagree on (§8). Adapter selection and the default (§9).
The conformance testkit (§10). The error taxonomy (§11).

### 1.5 Non-goals

- **1.5.1** Control plane: creating user pools, IAM administration,
  provider console configuration (parent §1.3).
- **1.5.2** Authorization policy. Roles, permissions and the default-closed
  policy algebra are primavera's (`primavera-spec.md` §8). The port exposes
  groups and claims and stops there.
- **1.5.3** Federation and enterprise protocols: SAML, LDAP, Kerberos. A
  provider that federates does so behind the port.
- **1.5.4** Passkeys and WebAuthn. Named as a capability for later, not
  specified here.
- **1.5.5** Cryptographic primitives. HMAC, CSPRNG, constant-time compare and
  asymmetric verification belong to the stdlib (§5.6).
- **1.5.6** Hosting login pages. The port is an API. Providers' hosted UIs
  are reachable by the application without the port.

### 1.6 Sequencing

Two cuts, matched to `primavera-web-spec.md` §1.6:

1. **Minimal** (with primavera's HTTP phase): §3 registration and lookup, §7
   attributes and groups, the memory driver, the testkit for those, §9
   selection.
2. **Full** (with primavera's security phase): §4 authentication, §5 tokens,
   §6 challenges, the remaining capabilities and testkit cases.

### 1.7 Systems

`cajeta.hash` (SHA-256, and the HMAC it still lacks), `cajeta.codec.Base64`,
`cajeta.codec.json`, `cajeta.time`, `cajeta.aot` (`@Component`, `@Inject`,
`@Profile`), `dev.cajeta.unit`. Adapters additionally use `cajeta-http` and
the provider's wire protocol.

---

## 2. Feature: the port model for identity

- **2.1** When code needs identity, it depends on the `identity` port in
  `cajeta-cloud` and on nothing provider-specific.
- **2.2** When the port is used, the operations are grouped by concern: a
  **user pool** (register, confirm, lookup, attributes, groups), an
  **authenticator** (sign in, respond to a challenge, refresh, sign out) and a
  **token verifier** (verify a token this pool issued, fetch its keys). A
  consumer that only registers users links only the pool.
- **2.3** When an operation takes user-supplied strings, the port copies them
  into its own storage. A caller's buffer, including a pooled request buffer
  (`primavera-web-spec.md` §3), is never retained by the port.
- **2.4** When an operation returns a user or a token, the value is owned by
  the caller (`#T`), never a window into driver state.
- **2.5** When the port is used from several fibers at once, each operation
  is atomic with respect to the others. Two concurrent registrations of one
  username yield exactly one success.

---

## 3. Feature: registration and lookup

- **3.1** When a user registers with a username, a password and optional
  attributes, the pool creates the account and returns a user id that is
  stable for the account's life.
- **3.2** When the username is already taken, registration fails with
  **user-exists** and creates nothing.
- **3.3** When the provider requires confirmation (a code sent by email or
  SMS), the account is created in an **unconfirmed** state, and `confirm(user,
  code)` completes it. A provider without confirmation reports the account
  confirmed on creation. Whether confirmation is required is a capability
  (§8).
- **3.4** When a password does not meet the provider's policy, registration
  fails with **policy-violation** and the message names the rule, never the
  password.
- **3.5** When a user is looked up by id or by username, the result is the
  user's id, username, confirmation state, attributes and groups, or
  **user-not-found**.
- **3.6** When a user is deleted, later lookups report **user-not-found** and
  later sign-ins fail with **invalid-credentials**.
- **3.7** When the memory driver confirms a user, the code it "sent" is
  available to tests through a driver-only hook. The driver never fakes a
  delivery channel.

---

## 4. Feature: authentication

- **4.1** When a user signs in with username and password, the result is one
  of: **tokens** (§5), or **challenge-required** naming the challenge (§6).
- **4.2** When the credentials are wrong, sign-in fails with
  **invalid-credentials**, and the message does not say which of the two was
  wrong.
- **4.3** When the account is unconfirmed, sign-in fails with
  **not-confirmed**.
- **4.4** When the provider has locked or throttled the account, sign-in
  fails with **locked**, carrying the retry-after the provider reports if
  any.
- **4.5** When a refresh token is presented, new tokens are issued, or the
  call fails with **invalid-token** if the refresh token is expired, revoked
  or unknown.
- **4.6** When a user signs out, the refresh token is revoked where the
  provider supports revocation (§8), and access tokens already issued remain
  valid until they expire. The port says so in its docs rather than pretending
  otherwise.
- **4.7** When the memory driver stores a password, it stores a salted hash,
  never the password. This is the reference implementation and it will be
  copied.

---

## 5. Feature: tokens and claims

- **5.1** When sign-in succeeds, the result carries an **access token**, an
  **id token** and, where the provider issues one, a **refresh token**, each
  with its expiry.
- **5.2** When an access or id token is issued, it is a JWT whose claims
  include the subject (the user id from §3.1), the issuer, the audience, the
  expiry, the username, and the user's groups. Custom attributes appear as
  claims under the provider's naming, and the port documents the mapping.
- **5.3** When a token is verified, the verifier checks signature, issuer,
  audience and expiry and returns the claims, or fails with
  **invalid-token** naming which check failed.
- **5.4** When a verifier needs keys, the port exposes the pool's signing
  keys as a JWKS document, fetched from the provider's discovery endpoint by
  an adapter and served from process memory by the memory driver.
- **5.5** When the memory driver issues a token, it signs with HMAC-SHA256
  under a per-process key and exposes that key through §5.4, so verification
  code is written once and runs unchanged against the memory driver and a
  provider.
- **5.6** When a real provider's token is verified, the signature is RS256 or
  ES256. That verification needs asymmetric primitives the stdlib does not
  have today. The stdlib asks, in order, are HMAC-SHA256, a CSPRNG over the
  `getrandom` source the runtime already uses for seeds, a constant-time
  compare, and RSA plus ECDSA P-256 verification. OpenSSL is already linked
  for TLS, so the last is an exposure, not a port. Until it lands, adapters
  are limited to opaque-token introspection, and the spec says so.
- **5.7** When a token is logged or reported in an error, it is redacted to
  its claims' subject and expiry. The signature and the raw token never
  appear (parent §9.5).

---

## 6. Feature: challenges

- **6.1** When sign-in answers **challenge-required**, the challenge names
  its kind: **TOTP**, **SMS-OTP**, **EMAIL-OTP**, **NEW-PASSWORD-REQUIRED**,
  or **MFA-SETUP**, and carries an opaque session handle.
- **6.2** When a challenge is answered with the session handle and the code,
  the result is tokens (§5) or **invalid-code** or **code-expired**.
- **6.3** When a user enrolls TOTP, the pool returns the shared secret once,
  the user's authenticator is verified with a first code, and the enrollment
  is only then active.
- **6.4** When a code is wrong more than the provider's limit, the challenge
  fails with **locked** (§4.4).
- **6.5** When the memory driver issues an SMS or email code, the code is
  available through the same test hook as §3.7. TOTP codes are computed in
  process from the enrolled secret and a `TimeSource`, so tests control the
  clock.

---

## 7. Feature: attributes and groups

- **7.1** When a user is created or updated, custom attributes are a map of
  string to string. The port does not type them. Providers that constrain
  attribute names or sizes report **policy-violation**.
- **7.2** When a user's attributes are updated, the update is whole-attribute:
  set or remove by name, never a partial string edit.
- **7.3** When a user is added to or removed from a group, later lookups and
  later tokens reflect it. Tokens already issued do not.
- **7.4** When groups are listed for a user, the result is the group names.
  Mapping groups to roles and permissions is primavera's (§1.5.2).

---

## 8. Feature: capabilities

Per parent §3: declared, queried before use, asserted at startup, never a
silent substitute.

- **8.1** When an adapter is queried, it declares each of: `CONFIRMATION`,
  `TOTP`, `SMS_OTP`, `EMAIL_OTP`, `REFRESH`, `REVOCATION`, `CUSTOM_ATTRIBUTES`,
  `GROUPS`, `JWKS`, `INTROSPECTION`, `PASSKEYS`.
- **8.2** When the memory driver is queried, it declares everything except
  `PASSKEYS` and `INTROSPECTION`, so the testkit exercises the full port
  against it.
- **8.3** When primavera's security phase starts up, it asserts the
  capabilities its configuration relies on (for example `TOTP` when OTP is
  enabled) and fails on launch if the adapter lacks one.

---

## 9. Feature: adapter selection and the default

primavera's DI posture (`primavera-spec.md` §16, decisions R2 and R3) is that
which implementations exist is decided at build, and which one a program uses
is ordinary selection over the compiled-in set. This port follows it. There
is no runtime scan for adapters.

- **9.1** When a provider adapter is a compiled dependency of the
  application, it contributes its implementation of the port to the DI graph
  as a named `@Component`.
- **9.2** When configuration names a provider (`identity.provider = cognito`),
  that named implementation is selected. When the name matches no compiled-in
  implementation, startup fails and the message lists the ones present.
- **9.3** When no provider adapter is compiled in, the memory driver is the
  only implementation and is selected without configuration. This is "default
  to the reference implementation".
- **9.4** When the active profile is not a development or test profile and
  the memory driver is the selection, startup fails unless configuration
  names `memory` explicitly. Users that vanish on restart are a deployment
  error, not a fallback.
- **9.5** When several adapters are compiled in, they are all available for
  selection, and selection can differ per environment through configuration
  alone (parent §2.2).

---

## 10. Feature: the conformance testkit

- **10.1** When an adapter is written, one suite exercises registration,
  lookup, authentication, challenges, tokens, attributes and groups, and the
  adapter passes or fails it (parent §10.1).
- **10.2** When the suite runs against an adapter, it tests each capability
  the adapter declares and asserts a clean failure for each it does not
  (parent §10.2).
- **10.3** When the suite tests registration, it runs concurrent registrations
  of one username and asserts exactly one success (§2.5).
- **10.4** When the suite tests tokens, it verifies a token through the
  port's own verifier and through an independent JWT check against the JWKS,
  so a driver cannot pass by agreeing with itself.
- **10.5** When the suite needs a delivered code, it uses the driver-only
  hook (§3.7), and a real adapter supplies the hook through a provider test
  facility or marks those cases as needing a live account.

---

## 11. Feature: errors

- **11.1** When an operation fails, the error is one of **user-not-found**,
  **user-exists**, **invalid-credentials**, **not-confirmed**,
  **challenge-required**, **invalid-code**, **code-expired**, **locked**,
  **invalid-token**, **policy-violation**, **unsupported-capability**,
  **transient**, or **provider-error** (parent §11.1 extended).
- **11.2** When a failure is transient, the parent's retry policy applies
  (parent §11.2). Sign-in is never retried automatically, since a retry is an
  attempt against a lockout counter.
- **11.3** When an error is reported, it names the provider and the
  operation. It never contains a password, a code, a secret or a raw token.

---

## 12. Open questions (resolve at plan time)

- **12.1** Does the minimal cut (§1.6) include §3.3 confirmation, or is
  confirmation part of the full cut? Recommendation: include it, since the
  registration sample should show the unconfirmed state rather than have it
  appear later as a surprise.
- **12.2** Which provider is the first real adapter? Cognito, since primavera
  names it and `cajeta-cloud-aws` already carries S3 as its first vertical.
  The adapter is its own spec.
- **12.3** Does the stdlib crypto work (§5.6) precede the full cut, or does
  the full cut ship against the memory driver only with RS256 verification
  deferred? Recommendation: HMAC-SHA256 and the CSPRNG land first, since the
  memory driver and session ids need them, and asymmetric verification lands
  with the Cognito adapter.
- **12.4** Password hashing in the memory driver: PBKDF2 over the SHA-256 the
  stdlib has, or wait for Argon2? Recommendation: PBKDF2-SHA256 now, with the
  parameters recorded in the stored hash so a later change does not strand
  existing users.

---

## 13. Acceptance criteria (spec-level)

- **13.1** The testkit passes against the memory driver, and every case that
  could pass against an always-succeeding driver has a companion that would
  fail against one (registration conflict, wrong password, expired code).
- **13.2** A consumer of the port links no provider SDK and no network
  capability (parent §14.7, §14.8).
- **13.3** Concurrent registrations of one username yield exactly one success
  under real concurrency (§2.5, §10.3).
- **13.4** A token issued by the memory driver verifies through an
  independent JWT check against its JWKS (§10.4).
- **13.5** With no adapter compiled in, the memory driver is selected without
  configuration in a test profile and refused in a production profile unless
  named (§9.3, §9.4).
- **13.6** No password, code, secret or raw token appears in any log line or
  error message, asserted by a test that greps the captured output (§11.3).
- **13.7** A pooled request buffer handed to `register` can be overwritten
  immediately after the call returns and the stored user is unaffected (§2.3).
