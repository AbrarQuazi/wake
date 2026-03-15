# Updated CAS Architecture

## Purpose

This document describes the current CAS architecture in the `wake-cas-prototype` branch after the latest FUSE/CAS changes.

It covers:

- the current object model
- the source and output data flow
- what `WAKE_CAS` means today
- what is intentionally deferred
- how to stage the rollout into Federation

This document supersedes the older mental model where only build outputs were stored in CAS. In the new model, **source inputs also enter CAS when `WAKE_CAS=1`**, which is what makes hash-addressed visible inputs work.

## Current Status

The code now supports a CAS-backed object model for:

- regular files
- symlinks

Wake `Path` values now also carry an explicit file type:

- `file`
- `symlink`
- `directory`

Directories are still represented as metadata-only items and are **not** stored as CAS objects yet.

Hardlinks and other special filesystem objects are **not supported** in CAS mode. In `WAKE_CAS=1`, they fail instead of being normalized into the CAS.

The feature is gated by `WAKE_CAS`. When the flag is not set, Wake keeps the legacy behavior.

## Core Model

### Object Identity

CAS identity is once again just "hash of stored bytes".

The important point is:

- regular files and symlinks use bare 64-hex hashes
- the digest is computed from the stored blob bytes only
- type is carried structurally in `Path.Type` and visible/staging JSON

The payload is:

- regular file: file contents
- symlink: symlink target bytes

This means:

- a regular file hashes its file contents
- a symlink hashes the symlink target path string stored as its blob
- a file and symlink may share a hash if their stored blob bytes are identical

This is the current model:

- `Path.Type` tells Wake what the object is
- `Path.Hash` is a plain content hash of the stored blob bytes
- the CAS store itself is keyed by a bare digest again

### Directory Handling

Directories are still outside the content-addressed object store. For now:

- directories use the existing all-zero hash sentinel
- directory mode is carried as metadata
- directory contents are not represented as CAS tree objects

This keeps the rollout small and avoids introducing Merkle tree semantics or additional DB changes in this area.

### Unsupported Types

In `WAKE_CAS=1`, these are unsupported and should fail:

- hardlinks
- FIFOs
- sockets
- block devices
- character devices
- any other non-regular, non-symlink, non-directory type

In legacy mode, Wake continues to behave the old way.

## Component Responsibilities

### 1. `wake-hash` / `shim-wake`

These tools define the hash seen by `Path.hash` and by visible-input serialization.

Legacy mode:

- regular files hash by raw contents
- symlinks hash by raw link target
- directories return the zero sentinel
- exotic files return the old fake exotic hash behavior

CAS mode:

- regular files are stored in `.cas` and return a bare content hash
- symlinks are stored in `.cas` as target-path blobs and return a bare content hash
- directories still return the zero sentinel
- unsupported types fail

This is the change that allows **source files to be ingested into CAS** before jobs run.

### 2. Wake `Path` / hash planning

Wake `Path` is now modeled as:

- path name
- path type
- hash

Visible-input serialization is now widened to:

- path
- type
- hash

In `WAKE_CAS=1`, the `hash` field is a bare 64-hex content hash over the stored blob bytes.

In this stage:

- `Path` carries explicit `file` / `symlink` / `directory` type information
- `computeHashes` propagates `WAKE_CAS` to the hash jobs
- `computeHashes` reuses cached `Path` entries when the DB metadata still matches and, in CAS mode, the referenced blob is still present in `.cas`
- `hashcode` reads the full hash line instead of assuming 64 hex characters

The local DB now persists both `hash` and `type`, so cached path reuse can remain enabled in CAS mode without losing the file/symlink distinction.

### 3. `fuse-waked`

The FUSE daemon is still responsible for isolated write staging during job execution.

In CAS mode it now does three distinct jobs:

- stages writes for job outputs under `.cas/staging`
- serves visible file and symlink inputs from `.cas/blobs` by explicit `type + hash`
- rejects unsupported filesystem operations for the current CAS model

Important behavior:

- `g_use_cas` now defaults to `false`
- `--use-cas` enables CAS mode
- visible regular files can be opened from CAS
- visible symlinks can be resolved from CAS through `readlink`
- `getattr` understands explicit file and symlink type plus bare hash
- hardlinks fail with `EPERM` in CAS mode
- `mknod` for special files fails in CAS mode

Workspace fallback still exists for visible reads when a CAS object is not available. That fallback is now a compatibility path, not the intended steady-state model.

### 4. Wakebox

Wakebox receives staged output metadata from the FUSE daemon and converts supported staged items into CAS-backed output metadata.

Current behavior:

- staged regular files are stored in CAS and emitted as `type=file, hash=<64hex>`
- staged symlinks are stored in CAS and emitted as `type=symlink, hash=<64hex>`
- staged directories are emitted as metadata-only directory entries
- hardlinks are rejected
- unsupported staged item types are rejected

Wakebox is therefore the point where job outputs move from staging files into durable CAS objects.

### 5. Wake runtime materialization

Wake receives the wakebox JSON and materializes the final workspace outputs.

Current behavior:

- file outputs materialize from file-content hashes
- symlink outputs materialize by reading the target-path blob and creating the symlink
- directories are created from metadata

Wake no longer needs the raw symlink target in the output JSON once wakebox has stored the symlink target in CAS.

## Data Flow

### Source Input Flow

This is the main architectural change from the earlier CAS design.

When `WAKE_CAS=1`, source inputs are added to CAS at hash time:

```text
source file or symlink
    |
    v
wake-hash / shim-wake
    |
    +--> regular file  -> store contents in .cas -> emit <64hex>
    |
    +--> symlink       -> store target in .cas   -> emit <64hex>
    |
    +--> directory     -> emit zero sentinel
    |
    +--> unsupported   -> fail
    |
    v
Path.hash
    |
    v
Path { name, type, hash }
    |
    v
runner JSON "visible": [{ path, type, hash }]
    |
    v
daemon_client -> fuse-waked Job::visible_types + Job::visible_hashes
```

When the job reads a visible input:

```text
job open/read/readlink/getattr
    |
    v
fuse-waked
    |
    +--> type=file, hash=<64hex>    -> read from .cas/blobs
    +--> type=symlink, hash=<64hex> -> read target bytes from .cas/blobs
    +--> no CAS hash                -> workspace fallback
```

The steady-state goal is that source visibility is hash-addressed through CAS, not through whatever happens to be present in the shared workspace.

### Output Flow

Job outputs still follow CAS-first staging:

```text
job writes output
    |
    v
fuse-waked
    |
    +--> regular file -> write to .cas/staging/<unique>
    +--> symlink      -> record virtual symlink in staged set
    +--> directory    -> record virtual directory in staged set
    +--> hardlink     -> fail in WAKE_CAS mode
    +--> special      -> fail in WAKE_CAS mode
    |
    v
job finishes
    |
    v
fuse-waked emits staging metadata JSON
    |
    v
wakebox
    |
    +--> file      -> store file in CAS -> emit bare content hash
    +--> symlink   -> store target in CAS -> emit bare content hash
    +--> directory -> emit metadata only
    |
    v
Wake runner post-processing
    |
    +--> file      -> materialize from CAS
    +--> symlink   -> materialize from CAS
    +--> directory -> mkdir + chmod
```

### Remote Execution Flow

The split remains:

- FUSE daemon stages locally to the execution environment
- wakebox stores outputs in CAS in that environment
- the execution result transfers CAS blobs plus JSON back to the local machine
- Wake materializes locally from CAS

For remote execution, the important effect of this design is that output processing is the same on local and remote jobs once wakebox has produced the explicit `type + hash` JSON.

## Why This Matters For Multi-Wake

Multi-wake requires that two Wake processes can safely operate in the same workspace without trusting mutable workspace paths as the source of truth for job inputs.

The current CAS design moves toward that by making visible inputs content-addressed:

- source inputs can be read from CAS by explicit `type + hash`
- outputs are staged privately and only materialized after completion
- the workspace becomes a materialization target, not the primary consistency mechanism

This is necessary for multi-wake, but it is not sufficient by itself.

The remaining multi-wake work is still outside this document's implemented scope:

- DB lock changes
- additional schema changes beyond `files.type`
- run/process-aware DB state
- CAS-aware persistent hash caching
- remote-cache hydration through CAS instead of direct workspace writes

## What Is Intentionally Deferred

### `wake.db` Schema

This stage **does** change the DB schema.

The current implementation bumps the schema to version 10 and adds `files.type`, so the local DB persists both the hash and the path type used by reflection and cached-path reuse.

That schema work is intentionally narrow. It does not yet redesign DB locking, run/process ownership, or broader multi-wake state management.

### DB Persistence Of `Path.Type`

This stage **does** persist `Path.Type` in `files.type`.

The migration and runtime still fill that type conservatively when needed:

- `lstat`
- the directory sentinel

but only for the supported transport set:

- `file`
- `symlink`
- `directory`

This is enough for the current rollout, but it is not the final multi-wake persistence model.

### Tree Objects

Directories are not CAS objects yet. There is no tree hashing or Merkle structure in this stage.

### Remote-Cache Hydration

Remote-cache hashing helpers now tolerate `WAKE_CAS`, but remote-cache hydration is still not CAS-native.

Today, remote-cache pull still writes directly into the workspace. That means:

- it may still "work" in some cases
- it is not the final multi-wake-safe CAS design

For the experimental multi-wake rollout, remote-cache pull should either be disabled or explicitly treated as unsupported until hydration is routed through CAS.

## Rollout Plan

### Stage 1: Official Wake Release

This is the safe release to ship broadly.

Include:

- plain content hashes for file contents and symlink target-path blobs
- explicit `Path.Type` for `file` / `symlink` / `directory`
- `wake.db` schema version 10 with persisted `files.type`
- visible-input transport widened to `{path, type, hash}`
- source hashing into CAS under `WAKE_CAS`
- FUSE visible reads by CAS hash
- output staging into CAS-backed objects
- symlink CAS materialization
- rejection of hardlinks and unsupported special files in CAS mode
- `WAKE_CAS` off by default

Do not include:

- DB lock removal
- process-aware DB changes
- claims that this is full multi-wake support

This release should be described as:

- CAS plumbing
- CAS-visible input support
- experimental opt-in via `WAKE_CAS`

### Stage 2: Federation Rollout Using Official Wake

In the Federation release PR:

1. bump the Wake pin
2. propagate `WAKE_CAS` into the runner environment
3. keep `WAKE_CAS` off by default
4. allow a small set of users or workflows to opt in

This gives Federation:

- a stable public Wake release
- opt-in access to the new CAS path
- only the shipped schema-10 migration for `files.type`, without the experimental DB redesign burden

This stage should be positioned as:

- experimental CAS mode
- not yet full multi-wake

### Stage 3: Experimental Multi-Wake Binary

This is the restricted beta path.

Build a separate experimental Wake variant that adds:

- DB lock redesign or removal
- further schema changes beyond `files.type`
- process/run-aware DB state
- CAS-aware persistent hash handling
- `computeHashes` / `add_hash` / `get_hash` semantics for the new world
- remote-cache hydration through CAS

This is the version that can be tested by the selected group of multi-wake users.

### Stage 4: Federation Beta With Experimental Wake

For the selected Federation users:

- switch them to the experimental Wake binaries
- enable `WAKE_CAS`
- optionally isolate them to separate workspaces or separate DB locations

Important warning:

- official and experimental binaries should not share the same `wake.db` once further experimental schema changes land

That means the experimental program either needs:

- a separate DB path, or
- isolated workspaces

## Release Guidance

### What To Say Publicly

For the official Wake release:

- `WAKE_CAS` enables an experimental CAS mode
- files and symlinks use plain content hashes with explicit transport type
- source visibility can be served from CAS
- hardlinks and special files are unsupported in CAS mode

Avoid saying:

- multi-wake is fully released
- the workspace database model is finalized
- remote-cache hydration is fully CAS-native

### What To Say For The Beta

For the selected Federation users:

- multi-wake testing requires the experimental binaries
- `WAKE_CAS` is necessary but not sufficient by itself
- the experimental DB/locking changes and further schema changes are part of the beta package

## Invariants

The current design relies on these invariants:

- content hashes are stable within CAS mode
- file/symlink interpretation comes from explicit `Path.Type`, not from the hash alone
- directories are not CAS objects in this stage
- hardlinks are not supported in CAS mode
- unsupported special file types fail in CAS mode
- the workspace is a materialization target, not the consistency source of truth

## File-By-File Changes

This section is intentionally implementation-oriented and may be removed later. It lists the files changed for the current CAS/FUSE stage and what each one now does.

### Build And Environment

`Makefile`

- links `wake-hash` against CAS objects
- links `shim-wake` against CAS objects
- keeps `fuse-waked` and `wakebox` building with the CAS code that now handles plain content hashes

`docker/Dockerfile.ubuntu-fuse`

- adds a simple Ubuntu 22.04 build image with FUSE headers and Wake native dependencies
- exists so the FUSE binaries can be compiled and smoke-checked on Linux from a macOS host

### CAS Core

`src/cas/content_hash.h`

- keeps content hashing over raw blob bytes
- keeps the directory sentinel helper

`src/cas/content_hash.cpp`

- hashes raw blob bytes without mixing in file type
- defines the directory zero-hash sentinel helper

`src/cas/cas.h`

- keeps the CAS surface mostly blob-oriented
- stores blobs by plain content hash
- keeps materialization as a regular-file operation; symlink creation happens in runtime/FUSE layers

`src/cas/cas.cpp`

- stores regular-file payloads by content hash
- stores symlink target bytes by content hash
- reads stored bytes by bare hash
- materializes regular files by bare hash
- leaves symlink materialization to higher layers that already know the explicit type

### Source Hashing And Wake Planning

`tools/wake-hash/main.cpp`

- switches from raw ad hoc hashing to the shared CAS/content-hash helpers
- when `WAKE_CAS` is off, keeps legacy hashing behavior
- when `WAKE_CAS` is on:
  - stores regular files in CAS and emits a bare content hash
  - stores symlinks in CAS as target-path blobs and emits a bare content hash
  - keeps directories as the zero sentinel
  - fails on unsupported special file types

`tools/shim-wake/main.cpp`

- mirrors the same hashing behavior as `wake-hash`
- delegates path classification and hashing/storage to `cas::resolve_path`
- stores file contents or symlink targets in CAS in `WAKE_CAS` mode

`share/wake/lib/system/path.wake`

- changes `Path` from `Name + Hash` to `Name + Type + Hash`
- exports `getPathType`
- adds `mkPath`, `pathTypeToString`, and `pathTypeFromString`
- limits transport types to `file`, `symlink`, and `directory`
- makes `isPathDir` prefer explicit type, with zero-hash fallback
- adds `useCasHashes` helper based on `WAKE_CAS`
- adds `addCasHashEnv` so hash jobs inherit `WAKE_CAS`
- makes `computeHashes` reuse cached `Path` values, including persisted type metadata, when the CAS blob still exists
- makes `computeHashes` fail fast if CAS hashing returns `BadHash`
- makes `hashcode` read the full line instead of assuming 64 characters
- propagates `WAKE_CAS` in `markFileCleanable`

`share/wake/lib/system/remote_cache_api.wake`

- updates `rscHashFile` so it also propagates `WAKE_CAS`
- stops assuming the hash output is always a raw 64-hex string
- does not change remote-cache hydration behavior yet

`share/wake/lib/system/job.wake`

- updates tree reflection decoding from `(path, hash)` to `(path, (type, hash))`
- rebuilds typed `Path` values from runtime reflection

`src/runtime/job.cpp`

- changes `job_tree` reflection output from `path + hash` to `path + (type, hash)`
- reserves and claims the additional type string in the runtime heap

`src/runtime/database.h`

- expands `FileReflection` to carry `path`, `type`, and `hash`

`src/runtime/database.cpp`

- reads stored `files.type` values directly for reflection
- normalizes empty stored types conservatively to `file`

### Wakebox And Runtime Materialization

`src/wakefs/fuse.cpp`

- changes wakebox post-processing to store staged regular files in CAS and emit bare content hashes
- changes staged symlink handling to store symlink targets in CAS and emit bare content hashes
- keeps directories metadata-only
- rejects hardlinks and unsupported staged item types in CAS mode
- keeps the wakebox-side timeout child naming fix Linux-only

`share/wake/lib/system/runner.wake`

- widens visible-input runner JSON from `{path, hash}` to `{path, type, hash}`
- serializes `Path.Type` explicitly for the FUSE daemon
- keeps file output materialization based on `hash`
- changes symlink output materialization to use a CAS hash instead of a raw symlink target
- keeps directory materialization metadata-only

`share/wake/lib/system/cas.wake`

- updates the comments and contract description for `casMaterializeItem`
- documents that file and symlink materialization now consume explicit type plus bare hash

`src/runtime/cas_prim.cpp`

- removes the old non-bare-hash parsing dependency from the CAS library boundary
- makes `cas_store_file` return a bare content hash
- makes `cas_has_blob` operate on bare hashes
- makes `cas_materialize_file` materialize file blobs by bare hash
- makes `cas_materialize_item` materialize:
  - file outputs from explicit `type=file` plus bare hash
  - symlink outputs by reading the stored target-path blob and creating the symlink
  - directories from metadata
- updates deprecated staging-ingest code to stay consistent with plain content hashes

### FUSE Daemon

`src/wakefs/fuse.h`

- expands visible-file transport objects to carry `path`, `type`, and `hash`

`src/wakefs/daemon_client.cpp`

- writes visible-input JSON as `{path, type, hash}` for the daemon

`src/wakefs/fuse.cpp`

- supports both legacy visible entries and the widened `{path, type, hash}` format
- infers a conservative type when older visible entries omit it
- keeps wakebox post-processing aligned with plain file-content and symlink-target hashes

`tools/fuse-waked/main.cpp`

- changes CAS mode to default off instead of on
- parses visible CAS state from explicit `type + hash`
- tracks visible type and visible hash separately
- adds helpers for reading CAS blob payloads and creating staged regular files
- updates `getattr` so visible files and symlinks can be served from CAS using explicit type
- updates `readlink` so visible symlinks can be served from CAS using explicit type
- updates `open` so visible file inputs open the CAS blob directly
- makes `create` and regular-file `mknod` write to staging in CAS mode
- rejects special-file `mknod` in CAS mode
- rejects hardlinks in CAS mode
- keeps workspace fallback for visible reads as a compatibility path

### Tests

`tests/runtime/symlinks/test.wake`

- adds assertions that claimed/source symlinks retain `PathTypeSymlink`
- checks that a regular file and symlink with similar payload content still carry different `Path.Type` values

## Files Not Changed In This Stage

These areas are intentionally not part of the current patch set:

- DB locking behavior
- broader process/run-aware DB semantics beyond the persisted `files.type`
- remote-cache hydration through CAS
- directory tree objects / Merkle representation

## Summary

The updated architecture is:

- **plain content-hash CAS blobs for file contents and symlink target-path strings**
- **source inputs enter CAS when hashed**
- **visible inputs can be served from CAS by hash**
- **outputs stage privately, then wakebox stores them in CAS**
- **Wake materializes final outputs from CAS**
- **directories remain metadata-only**
- **hardlinks and special files are rejected in CAS mode**

This is the right foundation for multi-wake, but the actual multi-wake release still requires a second stage with DB and remote-cache changes. The recommended Federation rollout is therefore:

1. ship the official Wake release with `WAKE_CAS` gated CAS plumbing
2. roll that into Federation as opt-in only
3. ship a separate experimental Wake binary with the DB/schema work
4. use that experimental binary for the limited multi-wake beta group
