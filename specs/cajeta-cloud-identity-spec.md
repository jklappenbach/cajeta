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
  attributes, the pool creates the account and returns the provider's
  **subject**, stable for the account's life. The subject is the provider's
  key, the value a token's `sub` claim carries. It is not the application's
  primary key: the application mints its own user id and stores the subject
  against it (`primavera-web-spec.md` §5.7), so a provider change re-links
  subjects instead of rekeying the system.
- **3.2** When the username is already taken, registration fails with
  **user-exists** and creates nothing.
- **3.3** When the provider requires confirmation (a code sent by email or
  SMS), the account is created in an **unconfirmed** state, and `confirm(username, code)` completes it, keyed by username
  like every public-plane call (§14.1). A provider without confirmation reports the account
  confirmed on creation. Whether confirmation is required is a capability
  (§8).
- **3.4** When a password does not meet the provider's policy, registration
  fails with **policy-violation** and the message names the rule, never the
  password.
- **3.5** When a user is looked up by subject or by username, the result is
  the user's subject, username, confirmation state, attributes and groups, or
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
  include the subject (§3.1), the issuer, the audience, the
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

- **12.1** *(resolved 2026-09-25, Julian: yes.)* Does the minimal cut (§1.6) include §3.3 confirmation, or is
  confirmation part of the full cut? Recommendation: include it, since the
  registration sample should show the unconfirmed state rather than have it
  appear later as a surprise.
- **12.2** *(resolved 2026-09-25: Cognito.)* Which provider is the first real adapter? Cognito, since primavera
  names it and `cajeta-cloud-aws` already carries S3 as its first vertical.
  The adapter is its own spec.
- **12.3** *(resolved 2026-09-25: HMAC-SHA256 and the CSPRNG first, asymmetric verify with the Cognito adapter.)* Does the stdlib crypto work (§5.6) precede the full cut, or does
  the full cut ship against the memory driver only with RS256 verification
  deferred? Recommendation: HMAC-SHA256 and the CSPRNG land first, since the
  memory driver and session ids need them, and asymmetric verification lands
  with the Cognito adapter.
- **12.4** *(resolved 2026-09-25, Julian: PBKDF2-SHA256.)* Password hashing in the memory driver: PBKDF2 over the SHA-256 the
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

---

## 14. Cognito analysis and the interface it implies

*Written 2026-09-25 at Julian's request, against the Amazon Cognito user
pools API reference read the same day (InitiateAuth, RespondToAuthChallenge,
SignUp, ConfirmSignUp, ListUsers, AdminAddUserToGroup, GlobalSignOut,
RevokeToken, the token and verification guides). Everything in §14 is
measured from the documents. §15 turns it into proposed amendments to §3
to §8 and §11, which are approved text and change only on approval.*

### 14.1 Three authorization planes, not one

Cognito splits its API by who is calling, and the split is the first thing
the port has to honour.

| Plane | Authorized by | Operations |
|---|---|---|
| **Public** | app client id, plus `SECRET_HASH` when the client has a secret | SignUp, ConfirmSignUp, ResendConfirmationCode, InitiateAuth, RespondToAuthChallenge, RevokeToken, ForgotPassword, ConfirmForgotPassword |
| **Self-service** | the user's own access token, scope `aws.cognito.signin.user.admin` | GetUser, UpdateUserAttributes, DeleteUser, ChangePassword, GlobalSignOut, AssociateSoftwareToken, VerifySoftwareToken, SetUserMFAPreference |
| **Administrative** | the adapter's own AWS credentials, SigV4 | AdminGetUser, ListUsers, AdminUpdateUserAttributes, AdminDeleteUser, AdminConfirmSignUp, AdminAddUserToGroup, AdminRemoveUserFromGroup, AdminListGroupsForUser, AdminCreateUser |

The minimal port shipped in Unit 1 (`UserPool`: lookup by id or name,
set and remove attributes, groups, delete, all keyed by user id and none by
token) is the **administrative plane**. On Cognito every one of those calls
needs the adapter's IAM credentials (parent §9.5), which a server has and a
browser must never have. The public plane is keyed by **username**, not by
id: SignUp, ConfirmSignUp and InitiateAuth all take `Username`. The
self-service plane is keyed by **access token** and has no user id at all.

`SECRET_HASH` is `Base64(HMAC-SHA256(clientSecret, username + clientId))`.
`cajeta.hash.HmacSha256` (Unit 0) is exactly what it needs.

### 14.2 Identity and registration

- The stable id is `sub`, returned by SignUp as `UserSub`. It is opaque and
  not to be validated as an RFC UUID. Matches §3.1.
- SignUp returns `UserConfirmed` and `CodeDeliveryDetails`: the delivery
  medium (`EMAIL`, `SMS`) and a **masked** destination (`m***@e***`). A
  registration result that cannot say where the code went makes the sample
  say nothing. `Password` is optional only for passwordless pools.
- Custom attributes are named `custom:<name>` on the wire and appear in the
  ID token under that name as strings. Standard attributes (`email`,
  `phone_number`, `given_name`, …) are bare. The port's attribute names are
  bare; the adapter adds and strips the prefix at its boundary.
- ConfirmSignUp is keyed by username and answers `CodeMismatchException`,
  `ExpiredCodeException`, `TooManyFailedAttemptsException`,
  `AliasExistsException` (an email or phone alias already belongs to another
  user) and `NotAuthorizedException` for an already-confirmed user. It also
  returns a `Session` that lets InitiateAuth sign the user in without a
  second code. ResendConfirmationCode exists and the port has no verb for it.
- Lookup by id on the administrative plane is `AdminGetUser` with the `sub`
  as `Username`, which the reference permits when username is not an alias
  attribute, or `ListUsers` with `Filter: sub = "<id>"`, which is
  **eventually consistent** and capped at 60 per page. Lookup by username is
  `AdminGetUser` directly. `ListUsers` filters only ten standard attributes
  and never custom ones.
- `UserStatus` values seen: `UNCONFIRMED`, `CONFIRMED`,
  `FORCE_CHANGE_PASSWORD`, `RESET_REQUIRED` and others. The port's boolean
  confirmed state is a projection of that.

### 14.3 Sign-in: flows and challenges

InitiateAuth takes an `AuthFlow` and answers either `AuthenticationResult`
or a `ChallengeName` with an opaque `Session` (20 to 2048 characters, echoed
unmodified) and `ChallengeParameters`.

| Flow | Port meaning | Note |
|---|---|---|
| `USER_PASSWORD_AUTH` | sign in with username and password | needs `ALLOW_USER_PASSWORD_AUTH` on the app client |
| `USER_SRP_AUTH` | password never leaves the client | needs SRP, which needs big-integer modular arithmetic the stdlib lacks (`uint128` is the widest type). Out of scope until a bignum lands. |
| `USER_AUTH` | choice-based: `PREFERRED_CHALLENGE` of `PASSWORD`, `PASSWORD_SRP`, `EMAIL_OTP`, `SMS_OTP`, `WEB_AUTHN`, or none to receive `AvailableChallenges` and a `SELECT_CHALLENGE` | Essentials tier or higher; the only route to passwordless and passkeys |
| `REFRESH_TOKEN_AUTH` | refresh | returns access and id tokens; **no refresh token** unless rotation is enabled |
| `CUSTOM_AUTH` | Lambda-defined challenges | surfaces as an opaque custom challenge |

Challenge names the port must map. The port's §6.1 set (TOTP, SMS-OTP,
EMAIL-OTP, NEW-PASSWORD-REQUIRED, MFA-SETUP) is short by three.

| Cognito | Port kind | Answer field |
|---|---|---|
| `SOFTWARE_TOKEN_MFA` | `TOTP` | `SOFTWARE_TOKEN_MFA_CODE` |
| `SMS_MFA`, `SMS_OTP` | `SMS_OTP` | `SMS_MFA_CODE`, `SMS_OTP_CODE` |
| `EMAIL_MFA`, `EMAIL_OTP` | `EMAIL_OTP` | `EMAIL_MFA_CODE`, `EMAIL_OTP_CODE` |
| `NEW_PASSWORD_REQUIRED` | `NEW_PASSWORD_REQUIRED` | `NEW_PASSWORD` plus `userAttributes.<name>` for each `requiredAttributes` entry |
| `MFA_SETUP` | `MFA_SETUP` | a session from VerifySoftwareToken; parameters carry `MFAS_CAN_SETUP` |
| `SELECT_MFA_TYPE` | **`SELECT_FACTOR`** (new) | `ANSWER` = one of `SMS_MFA`, `EMAIL_MFA`, `SOFTWARE_TOKEN_MFA` |
| `SELECT_CHALLENGE` | **`SELECT_FACTOR`** (new) | `ANSWER` = one of `AvailableChallenges`, plus that factor's own parameters |
| `CUSTOM_CHALLENGE` | **`CUSTOM`** (new) | `ANSWER`; parameters are the Lambda's |
| `WEB_AUTHN` | `PASSKEY` | `CREDENTIAL`, a WebAuthn `AuthenticationResponseJSON` |
| `PASSWORD`, `PASSWORD_SRP`, `PASSWORD_VERIFIER`, `DEVICE_SRP_AUTH`, `DEVICE_PASSWORD_VERIFIER` | never surfaced | the adapter answers these itself |

Every challenge response also carries `USERNAME` and, with a client
secret, `SECRET_HASH`. `PASSWORD_VERIFIER` must be answered within seconds
or it fails as `NotAuthorizedException`.

TOTP enrollment during `MFA_SETUP` is a three-call dance: AssociateSoftwareToken
with the challenge `Session` returns the secret, VerifySoftwareToken with the
first code returns a new `Session`, and RespondToAuthChallenge `MFA_SETUP`
with that session completes sign-in. After sign-in the same two calls take
the access token instead. §6.3 covers the second case only.

### 14.4 Tokens, claims and verification

- `AuthenticationResult`: `AccessToken`, `IdToken`, `RefreshToken`,
  `ExpiresIn` (seconds, one value for access and id), `TokenType: Bearer`,
  `NewDeviceMetadata`. Matches §5.1.
- Access and id tokens are signed by **different RSA keys** with different
  `kid` values. Both are `RS256`. The JWKS is at
  `https://cognito-idp.<region>.amazonaws.com/<poolId>/.well-known/jwks.json`,
  keys carry `kid`, `kty: RSA`, `n`, `e`, `use: sig`. Keys rotate: cache by
  `kid`, refresh on an unknown `kid` from the right issuer.
- Issuer: `https://cognito-idp.<region>.amazonaws.com/<poolId>`.
- Access token claims: `sub`, `cognito:groups`, `iss`, `client_id`,
  `token_use: access`, `scope`, `username`, `jti`, `origin_jti`, `auth_time`,
  `exp`, `iat`, and `aud` only with resource binding. Verification checks
  `client_id`, not `aud`.
- ID token claims: `sub`, `cognito:groups`, `cognito:username`, `aud` (the
  client id), `token_use: id`, `email`, `email_verified`, standard OIDC
  claims, `custom:<name>` as strings, `identities` for federated users.
- The verification recipe is: structure, `kid` against the JWKS, signature,
  `exp`, `iss`, `aud` or `client_id` by token use, `token_use`. Revocation is
  invisible to an offline check: `origin_jti` ties access and id tokens to
  their refresh token, and only Cognito's own APIs honour a revocation.
- Groups appear in **both** tokens. §5.2 holds.
- RS256 verification is an RSA PKCS#1 v1.5 SHA-256 check over `n` and `e`
  from the JWKS. The stdlib ask in §5.6 is precisely that primitive. Nothing
  else blocks a Cognito adapter.

### 14.5 Sign-out and revocation

- `RevokeToken(ClientId, ClientSecret?, Token)` revokes **one refresh token**
  and the access and id tokens issued with it. The app client must have
  token revocation enabled, else `UnsupportedOperationException`.
- `GlobalSignOut(AccessToken)` invalidates every refresh token of the user
  and stops Cognito's own token-authorized APIs accepting their access
  tokens. Tokens presented to a resource server verifying offline remain
  valid until `exp`. §4.6 already says so; the port needs both verbs.

### 14.6 Lockout, throttling, and what the message may say

- Five wrong passwords lock sign-in with exponential backoff up to about
  fifteen minutes; the error is `NotAuthorizedException` with a message
  saying attempts were exceeded. There is **no retry-after value**. §4.4's
  "if the provider reports one" is right and Cognito does not.
- `TooManyRequestsException` and `LimitExceededException` are throttles,
  and SignUp can succeed with `LimitExceededException` when the code could
  not be sent: the user exists, unconfirmed, and needs ResendConfirmationCode.
- With "prevent user existence errors" on, an unknown user signing in gets
  `NotAuthorizedException`, the same as a wrong password. §4.2 holds and the
  memory driver must behave the same way.

### 14.7 Error mapping

| Cognito exception | Port kind |
|---|---|
| `UsernameExistsException`, `AliasExistsException` | `USER_EXISTS` |
| `UserNotFoundException` | `USER_NOT_FOUND` (never surfaced from sign-in, §14.6) |
| `NotAuthorizedException` on sign-in or a challenge | `INVALID_CREDENTIALS`, or `LOCKED` when the message says attempts exceeded |
| `NotAuthorizedException` on ConfirmSignUp of a confirmed user | success, idempotent |
| `UserNotConfirmedException` | `NOT_CONFIRMED` |
| `PasswordResetRequiredException` | **`PASSWORD_RESET_REQUIRED`** (new) |
| `CodeMismatchException` | `INVALID_CODE` |
| `ExpiredCodeException` | `CODE_EXPIRED` |
| `TooManyFailedAttemptsException` | `LOCKED` |
| `TooManyRequestsException`, `LimitExceededException`, `InternalErrorException` | `TRANSIENT` |
| `InvalidPasswordException`, `PasswordHistoryPolicyViolationException`, `InvalidParameterException` on a user-supplied value | `POLICY_VIOLATION` |
| `UnauthorizedException`, `UnsupportedTokenTypeException` | `INVALID_TOKEN` |
| `MFAMethodNotFoundException`, `SoftwareTokenMFANotFoundException`, `UnsupportedOperationException`, `OperationNotEnabledException` | `UNSUPPORTED_CAPABILITY` |
| `ResourceNotFoundException` (pool or client), `InvalidUserPoolConfigurationException`, `ForbiddenException`, Lambda exceptions | `PROVIDER_ERROR` |

Cognito returns every one of these as HTTP 400 except `InternalErrorException`
(500); the exception name travels in `__type`. Status codes carry nothing.

### 14.8 Wire protocol

JSON over HTTPS, `POST` to `cognito-idp.<region>.amazonaws.com/`, header
`X-Amz-Target: AWSCognitoIdentityProviderService.<Operation>`, content
type `application/x-amz-json-1.1`. Public and self-service calls are
unsigned. Administrative calls are SigV4, HMAC-SHA256 over a canonical
request, which the S3 adapter in `cajeta-cloud-aws` needs anyway, so the
signer is shared. `cajeta-http`'s client, `cajeta.codec.json` and
`cajeta.hash.HmacSha256` cover the whole adapter. No SDK.

### 14.9 What the adapter declares

`CONFIRMATION`, `CUSTOM_ATTRIBUTES`, `TOTP`, `SMS_OTP`, `EMAIL_OTP`,
`REFRESH`, `JWKS` always. `GROUPS` and the administrative plane only when
AWS credentials are configured. `REVOCATION` only when the app client enables
it. `PASSKEYS` only on the Essentials tier with `USER_AUTH` allowed.
`INTROSPECTION` never: Cognito has no RFC 7662 endpoint, and its `userInfo`
endpoint serves hosted-UI tokens only.

---

## 15. Proposed amendments (pending approval)

Each item names the section it changes. Nothing here is in force until
Julian approves it.

- **15.1 (§2.2) Four concerns, not three.** `UserPool` is the
  administrative plane and stays as shipped. `Authenticator` is the public
  plane (sign in, challenges, refresh, sign out, forgot password).
  **`Account`** (new) is the self-service plane, every call keyed by the
  caller's access token: `me`, `updateAttributes`, `changePassword`,
  `deleteMe`, `enrollTotp`, `signOutEverywhere`. `TokenVerifier` is
  unchanged. primavera's `GET /users/me` is `Account.me`, not a `UserPool`
  lookup, so a request never needs server credentials to read its own user.
- **15.2 (§3.3, §3.7) Confirmation is keyed by username**, as the public
  plane is: `confirm(username, code)` *(approved 2026-09-25, landed in cloud
  with 15.17)* and a new `resendConfirmationCode(username)` *(pending)*.
  `RegistrationResult` gains `codeDelivery()`: medium (`EMAIL`, `SMS`,
  `NONE`) and a masked destination. The memory driver masks like Cognito.
  Unit 1's `confirm(userId, code)` changed with this item.
- **15.17 (§3.1, §3.5) The port's id is the provider's subject** *(approved
  2026-09-25, Julian: the application keeps primary-key ownership)*. `userId`
  is renamed `subject` throughout the port, `lookupById` becomes
  `lookupBySubject`, and the application's own user id lives in primavera's
  user directory (`primavera-web-spec.md` §5.7, §5.8), never in this port.
- **15.3 (§4.1) Sign-in names a factor.** `signIn(username, password)` stays
  and `signInWith(username, factor)` is added, `factor` one of `PASSWORD`,
  `EMAIL_OTP`, `SMS_OTP`, `PASSKEY`. On Cognito the first maps to
  `USER_PASSWORD_AUTH` or to `USER_AUTH` with `PREFERRED_CHALLENGE: PASSWORD`
  by configuration, the second to `USER_AUTH`. A driver without a factor
  fails with `UNSUPPORTED_CAPABILITY`.
- **15.4 (§6.1) Challenge kinds** become `TOTP`, `SMS_OTP`, `EMAIL_OTP`,
  `NEW_PASSWORD_REQUIRED`, `MFA_SETUP`, `SELECT_FACTOR`, `CUSTOM`, `PASSKEY`.
  A `Challenge` carries its kind, the opaque session, a string-to-string
  parameter map (masked destination, the factors that can be set up, the
  required attributes) and, for `SELECT_FACTOR`, the options.
- **15.5 (§6.2) One answer shape.** `respond(session, kind, ChallengeAnswer)`
  where the answer holds a value (a code, a new password, a chosen factor, a
  custom answer, a passkey assertion) and an optional `Attributes` for the
  required attributes of `NEW_PASSWORD_REQUIRED`.
- **15.6 (§6.3) TOTP enrollment in both places**: `Account.enrollTotp()`
  after sign-in, and `Authenticator.enrollTotpForChallenge(session)` inside
  `MFA_SETUP`. Both return the secret once and are completed with a first
  code, the second returning the session that answers the challenge.
- **15.7 (§4.5) Refresh may return no refresh token.** `Tokens.refreshToken()`
  is empty then and the caller keeps the one it has. With rotation the new
  one is returned and the old one is dead.
- **15.8 (§4.6) Two sign-outs.** `Authenticator.signOut(refreshToken)`
  revokes one session, needs `REVOCATION`. `Account.signOutEverywhere()`
  invalidates every session of the caller.
- **15.9 (§4, new) Password reset.** `forgotPassword(username)` delivers a
  code and `confirmForgotPassword(username, code, newPassword)` completes it.
  `Account.changePassword(old, new)` for a signed-in user. A new error kind
  `PASSWORD_RESET_REQUIRED` for a sign-in the provider refuses until the
  flow runs.
- **15.10 (§5.2, §5.3) Claims and verification.** `Claims` exposes
  `subject`, `issuer`, `audience`, `tokenUse` (`ACCESS`, `ID`), `username`,
  `groups`, `scope`, `expiresAt`, `issuedAt`, `attributes()` for an id token
  with provider prefixes stripped, and `claim(name)` for anything else.
  `TokenVerifier.verify(token, expectedUse)` checks signature, issuer,
  audience or client id by use, expiry and use, and names the failing check.
  It caches keys by `kid` and refetches the JWKS once on an unknown `kid`.
- **15.11 (§7.1, new §7.5) Attribute names are bare in the port.** A
  provider prefix such as `custom:` is the adapter's, added on the way out
  and stripped on the way in, in tokens too.
- **15.12 (§8.1) Capabilities** gain `ADMIN` (the administrative plane is
  available) and `PASSWORD_RESET`. The memory driver declares both.
- **15.13 (§11.1) Kinds** gain `PASSWORD_RESET_REQUIRED`.
- **15.14 (§4.2, §13.1) The memory driver hides user existence at sign-in**
  by default, like Cognito with the option on: unknown user and wrong
  password answer the same `INVALID_CREDENTIALS` with the same message.
- **15.15 (§5.6) The stdlib ask narrows** to RSA PKCS#1 v1.5 SHA-256
  verification over `n` and `e`, since that is all Cognito signs with. ES256
  and SRP move to "when a provider needs them".
- **15.16 (primavera-web §8.1)** The sample's confirm route becomes
  `POST /users/{username}/confirm`, `GET /users/me` reads through `Account`,
  and `POST /login` answers 200 with tokens or 202 with the challenge kind,
  session and parameters.
