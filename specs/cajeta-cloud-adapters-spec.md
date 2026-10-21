# cajeta-cloud adapters: Cognito, S3, and the local object store (spec)

Status: draft, 2026-10-21, scoped at Julian's "do those"; awaiting approval.
Parent specs: `cajeta-cloud-spec.md` (§1.4, §1.5, §9, §10, §13.3 to §13.5, §14)
and `archive/cajeta-cloud-identity-spec.md` (§8 capabilities, §9 selection).
This spec adds only what those leave open; it restates nothing they decide.

## 1. Definition

`dev.cajeta.cloud` 0.4.0 ships three ports (object store, stream, identity),
their in-memory drivers and the conformance testkit, and no backend that
survives a restart or reaches a provider. Three artifacts close that gap, each
passing the parent spec's conformance suite (§1.5.2):

- **`cajeta-cloud-aws`** (`dev.cajeta.cloud.aws`): the Cognito user-pool
  adapter for the identity port and the S3 adapter for the object-store port,
  over cajeta-http, signed with SigV4, declaring the network capability (§9.6).
- **`cajeta-cloud-local`** (`dev.cajeta.cloud.local`): a filesystem object
  store, declared **dev-grade** (§1.5.1), declaring the filesystem capability.
- **`dev.cajeta.codec` 0.9.0**: the XML reader the S3 adapter parses with
  (§13.4), a second consumer being `cajeta-docs`.

The order is Cognito, S3, local: the identity adapter is what the auth work in
primavera waits on; the S3 adapter is the first real deployment target
(§13.3); the local store needs no account and unblocks nothing but itself.

## 2. The AWS adapters

- **2.1 Credentials** (parent §9.5). An adapter resolves credentials in this
  order and stops at the first that yields a key: an explicit `AwsConfig`
  component the application declares; the environment
  (`AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, optional
  `AWS_SESSION_TOKEN`, `AWS_REGION`); the shared credentials file
  (`~/.aws/credentials`, profile from `AWS_PROFILE` or `default`). Instance
  and container metadata credentials are a later addition, named here so the
  chain's shape admits them. A missing key fails at configuration time with
  the chain's steps named; no step is logged with its value.
- **2.2 Signing.** Every request is signed with SigV4 over cajeta-http's
  `ClientRequest`: canonical request, string to sign, signing key derived
  through cajeta-crypto's HMAC-SHA256, `x-amz-content-sha256` set
  (`UNSIGNED-PAYLOAD` for streamed bodies S3 permits it for). The signer is
  proven against AWS's published SigV4 test-suite vectors, which need no
  account.
- **2.3 Cognito.** `CognitoUserPool` implements `UserPool`, `Authenticator`,
  `Account` and `TokenVerifier` over the `cognito-idp` JSON API
  (`X-Amz-Target: AWSCognitoIdentityProviderService.<Op>`). Sign-in uses
  `USER_PASSWORD_AUTH` on an app client that permits it (SRP is not offered;
  the flow is a client-side proof the port does not model); TOTP enrollment
  and challenge map to `AssociateSoftwareToken`, `VerifySoftwareToken` and
  `SOFTWARE_TOKEN_MFA`; refresh to `REFRESH_TOKEN_AUTH`; revocation to
  `RevokeToken`; registration to `SignUp` and `ConfirmSignUp`; attributes and
  groups to the admin operations, which are SigV4-signed, while the user-facing
  operations are unsigned as the API defines. Tokens verify against the pool's
  JWKS (`/.well-known/jwks.json`, RS256) through the port's existing
  verifier, cached for the document's `Cache-Control` lifetime and refetched
  on an unknown `kid`. The adapter declares exactly the capabilities the pool
  configuration grants (identity §8); the selection name is `cognito` (§9.2).
- **2.4 S3.** `S3ObjectStore` implements the object-store port over the REST
  API: `PutObject` with `If-None-Match: *` and `If-Match` for conditional
  writes (parent §9.2), `GetObject` with `Range`, `ListObjectsV2` with
  prefix, delimiter and continuation, `DeleteObject`, and multipart
  (`CreateMultipartUpload`, `UploadPart`, `CompleteMultipartUpload`,
  `AbortMultipartUpload`). Bodies stream through cajeta-http in both
  directions; nothing larger than one part is materialized. Addressing is
  virtual-host style with path style as a configured fallback for S3
  compatible endpoints. Error bodies parse to the parent's error taxonomy
  (§11.1) by S3 error code.
- **2.5 Acceptance against the real services** (parent §14.1, §14.2). The
  conformance suite runs against a real user pool and a real bucket when the
  environment names them (`CAJETA_AWS_TEST_POOL`, `CAJETA_AWS_TEST_CLIENT`,
  `CAJETA_AWS_TEST_BUCKET`, with the credentials of 2.1), and reports skipped
  otherwise; the SigV4 vectors and recorded response fixtures run always. The
  real-service legs are an opt-in workflow with the account's secrets, never
  the default CI.

## 3. The XML reader (`dev.cajeta.codec.xml`)

- **3.1** A pull reader over bytes: start element with attributes, text,
  end element, with namespace prefixes kept as written and the five predefined
  entities plus numeric references decoded. No DTD processing, no external
  entities, a declared size limit; a malformed document throws with the byte
  offset. That is what S3 and OOXML need, and nothing else is promised.
- **3.2** A small writer for request bodies (`CompleteMultipartUpload`,
  `Delete`), escaping text and attribute values.

## 4. The local object store

- **4.1** `FilesystemObjectStore` over a root directory: each key a file
  under the root with its metadata (etag, content type, user metadata) in a
  sidecar; puts write to a temp file and rename; conditional writes hold a
  per-key lock in the process and compare the stored etag, which makes them
  correct for one process and declared so: the driver's grade is
  **dev-grade** (parent §1.5.1), stated in its capability document and its
  README. Listing walks the root with prefix and delimiter semantics matching
  the port. Range reads seek.
- **4.2** It passes the conformance suite, concurrent conditional writes
  included, with no account and no network, on Linux, macOS and Windows path
  rules.
- **4.3** A deployable grade (multi-process safety through file locks) is not
  promised here; the capability document names the grade so a deployment
  needing more fails at configuration time (§1.5.3).

## 5. Open questions

- **5.1** The AWS test account: whose, and how its keys reach the opt-in
  workflow and a developer's shell (2.5).
- **5.2** Whether `USER_PASSWORD_AUTH` alone is acceptable for Cognito sign-in
  in the first release, or SRP is required for pools that forbid the password
  flow.
