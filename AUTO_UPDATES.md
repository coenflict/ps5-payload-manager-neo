# Automatic Updates

Payload Manager keeps the payloads it launches current by itself. At every
start it checks each payload in your autoload list against the release its
maintainer published, installs a verified newer version into the same slot,
and only then launches that entry. When there is no network, a source is down,
a download fails verification or the check takes too long, the version that is
already installed is launched instead. Nothing on your PC is involved.

```
WebKit Autoloader ─► jailbreak ─► Payload Manager (your build)
                                   │ 1. newer build of *this* fork ready? → hand off
                                   │ 2. HTTP server + browser start
                                   │ 3. update worker ──────────────┐
                                   │ 4. autoload countdown           │ catalog refresh
                                   │ 5. for each autoload entry:     │ per payload, in sequence order:
                                   │      wait for its check ◄───────┤   maintainer release (GitHub/Gitea)
                                   │      (bounded by the budget)    │   curated catalog
                                   │      launch                     │   verify → install → keep previous
                                   │ 6. boot guard probation (90 s)  └ other payloads, manager channel
```

## What happens at startup

1. **Crash check.** If the previous boot stopped while an *updated* payload was
   being launched (or within 90 seconds after the sequence finished), that
   payload is restored to its previous version and the failing version is
   skipped from then on. Payloads that were not updated are never touched.
2. **Manager handoff.** If a newer build from this manager's own release
   channel was downloaded earlier, the manager hands off to it (see
   [Self-updates](#payload-manager-self-updates)).
3. **Update pass.** A background worker refreshes the catalogs and then checks
   payloads in autoload order, so the first payload of your sequence is ready
   first. Payloads that are installed but not in the autoload list are checked
   afterwards, at most twice a day.
4. **Autoload.** The countdown runs in parallel. Before each entry launches,
   autoload waits for that entry's own check to finish, at most until the
   *budget* (default 20 s, counted from startup) is used up. If an update
   arrives after the budget, it is installed for the next boot.

Typical overhead with a working connection is a second or two, mostly hidden
behind the countdown. Repeated boots within 10 minutes reuse the previous
answer without any request.

## Where versions come from

For each payload Payload Manager compares two sources and takes the highest
acceptable version:

| Source | How it is found | Integrity |
| --- | --- | --- |
| **Maintainer release** (upstream) | The catalog's `source`/`source_direct` links (or your override) name the GitHub or Gitea/Forgejo repository. The release asset is matched by a pattern derived from the asset the catalog uses, e.g. `ps5upload-*.elf` or `kstuff.elf`. | GitHub's published SHA-256 `digest` for the asset |
| **Catalog** (the mirror or your custom sources) | The cached repository entry with the same id | The catalog `checksum` |

This makes updates *upstream-first*: a new release is picked up directly from
its maintainer even before the curated mirror updates, while the mirror still
covers payloads whose upstream ships an archive (for example a `.zip`) or no
checksum. When the upstream download fails or fails verification, the catalog
copy is tried next if it is also newer.

Every download is then checked again before it is installed:

- SHA-256 must match the published digest/checksum;
- `.elf` files must be x86-64 ELF64 executables, and `.bin` files must not be an
  HTML/JSON error page;
- the replaced payload and its metadata are kept in the slot (`.previous`) and
  the slot is replaced atomically, so a failure leaves the installed version
  untouched.

Releases on GitHub are requested with `If-None-Match`; unchanged answers (HTTP
304) do not count against GitHub's anonymous limit of 60 requests per hour. If
the limit is reached anyway, upstream checks pause until GitHub's reset time
and the catalog keeps working. If GitHub is unreachable, it is skipped for a
minute instead of costing one timeout per payload.

## Choosing what gets installed

Open **Settings → Automatic Updates**. Each installed payload has a policy:

| Policy | Behaviour |
| --- | --- |
| **Safe** (default) | Installs new versions automatically, but a change of the major version (`1.x` → `2.0`) waits for your approval. |
| **Automatic** | Installs every newer verified version. |
| **Notify only** | Shows available versions; you install them with one click. |
| **Off** | Never checked. |

Further protections:

- **Stable only.** Releases flagged as prerelease, and tags containing words
  such as `alpha`, `beta`, `rc`, `pre`, `dev`, `test` or `nightly`, are ignored
  unless you tick *Include prereleases* for that payload — or you already run a
  prerelease of it. Version suffixes like `2.5B` are not treated as prereleases.
- **Restore.** *Restore vX* swaps the previous version back in. The version you
  restored away from is skipped until you click *Allow again* or a newer
  version appears.
- **Approval.** *Install vX* installs a version that a policy held back.
- **Advanced.** Override the upstream repository (`owner/repo` or a release
  URL) and the asset pattern, allow prereleases, or allow releases that publish
  no checksum. Uploaded payloads (not installed from a catalog) can be tracked
  this way too.

Firmware compatibility is not published in a machine-readable way by any
payload, so it cannot be checked automatically. For firmware-sensitive kernel
payloads (kstuff, etaHEN, Linux loaders) the *Safe* default plus the crash
guard is a sensible baseline; use *Notify only* if you prefer to read release
notes before every update.

## Payload Manager self-updates

Every build embeds the GitHub repository whose releases may update it
(`PLDMGR_CHANNEL:<owner/repo>`). A build only hands off to, or downloads, a
manager carrying the **same** channel marker:

- this fork follows `coenflict/ps5-payload-manager` releases and ignores
  official builds — including official update files already on the console and
  the official catalog's `pldmgr` entry;
- official builds carry no marker, so they never pick up a fork build either.

New manager releases are downloaded in the background (SHA-256 digest, embedded
version and channel must all match) into `/data/pldmgr/self/` and start the next
time the manager is launched. If a new build fails to come up after the
handoff, the next start notices, stays on the working build and skips that
version.

## One-time setup with WebKit Autoloader

WebKit Autoloader runs the *unified autoloader*, which launches the official
Payload Manager embedded in it unless it finds an `autoload.txt`. To run this
fork instead:

1. **Publish a release of the fork.** On GitHub open *Actions → Release
   On-Demand → Run workflow*. It builds `pldmgr_vX.Y.Z.elf` with the update
   channel set to the repository it runs in, and attaches it to release
   `vX.Y.Z`. (For later releases, bump `MENU_VERSION` in `include/pldmgr.h`
   first; the workflow refuses to overwrite an existing tag.)
2. **Install the bootstrap copy.** Download that ELF, rename it to
   `pldmgr.elf`, and put it on a USB drive like this:

   ```text
   USB root/
   └── ps5_autoloader/
       ├── autoload.txt
       └── pldmgr.elf
   ```

   with `autoload.txt` containing:

   ```text
   @sync
   pldmgr.elf
   ```

   Launch WebKit Autoloader once with the drive inserted. After the manager has
   started, `@sync` moves the folder to `/data/ps5_autoloader/` and removes it
   from the USB drive. (Without `@sync` support in your autoloader, copy the
   folder to `/data/ps5_autoloader/` with an FTP payload, or leave the drive
   inserted.)
3. **Configure once in the web UI** (`http://<PS5-IP>:8084`):
   - install your payloads from **Get Payloads**, so each one knows its source;
     replace payloads you uploaded manually, or set their upstream repository
     under *Automatic Updates → Advanced*;
   - put them in order under **Autoload** (delays such as `!4000` still work)
     and enable it;
   - review the policies under **Settings → Automatic Updates**.

From then on the flow is: launch WebKit Autoloader → jailbreak → this manager
→ (newer manager build? hand off) → payload updates → your sequence.

The bootstrap copy in `/data/ps5_autoloader/` never needs to change; newer
builds are picked up from `/data/pldmgr/self/`. Replacing the bootstrap copy
occasionally saves the short handoff (about a second) at each start.

To return to the official manager, delete `/data/ps5_autoloader/` (or its
`autoload.txt`). Your payloads, autoload list and settings are shared.

## Limitations

- **Hardware validation.** The update logic is tested on a host against a local
  GitHub/catalog fixture (see `tests/run_updater_tests.py`), and all sources
  compile and link against the PS5 SDK libc without warnings, but it has not run
  on a console yet. Watch the first boots once in **Settings → Open Log Viewer**
  (`[Updater]`, `[Autoload]`, `[BootGuard]`, `[Channel]` lines).
- **Archives.** Upstream assets packed as `.zip`/`.7z` are not extracted; those
  payloads update through the catalog.
- **Checksums.** Assets uploaded to GitHub before mid-2025 and most non-GitHub
  forges publish no SHA-256 digest; such releases are only installed when you
  allow it for that payload, otherwise the catalog's verified copy is used.
- **Ambiguous assets.** If a release contains several files matching the
  pattern and none of them is exactly the release tag substituted into it, the
  release is skipped rather than guessed. Set an asset pattern under
  *Advanced* to choose.
- **Crash detection is a heuristic.** Turning the console off within 90 seconds
  after an autoload that installed updates looks like a crash and restores the
  previous versions of those payloads (and skips the new versions until you
  allow them again).
- **Manager updates** take effect at the next start, not mid-boot.
- **No authentication.** As before, anyone on your LAN who can reach port 8084
  can manage payloads, including their update sources. Keep the console on a
  trusted network.
- Translations of the new screens exist for English and Dutch; other languages
  show English until translated.

## Files

| Path | Content |
| --- | --- |
| `/data/pldmgr/updates.json` | Update settings and per-payload policies/overrides |
| `/data/pldmgr/upstream/` | Cached release lists, ETags, rate-limit pause |
| `/data/pldmgr/boot_guard.txt` | Launch state used by the crash check |
| `/data/pldmgr/self/` | Downloaded manager builds and handoff health state |
| `/data/pldmgr/payloads/{repo,local}/<slot>/.previous/` | Previous version of an updated payload |

## API

| Route | Method | Purpose |
| --- | --- | --- |
| `/updates_status` | GET | Settings, manager channel and per-payload state |
| `/updates_check` | POST | Start a manual check (fresh conditional requests) |
| `/updates_apply?slot=` | POST | Install the available version now |
| `/updates_rollback?slot=` | POST | Restore the previous version and skip the replaced one |
| `/updates_settings` | POST | Partial JSON update: `enabled`, `budget` (0–300 s), `self_update`, `payloads: {"<slot>": {policy, prerelease, unverified, repo, asset, skip} \| null}` |

The POST routes follow the same origin/marker policy as all other actions.
`/autoload_status` additionally reports `"updating": true` while the sequence
waits for an entry's update.
