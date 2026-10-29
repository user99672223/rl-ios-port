# Auth and launch: Legendary in embedded CPython (spec 6.7, step 8)

Scope: `ios/legendary/` (bridge), `ios/Auth/` (WKWebView login), `tools/cpython/` (CPython for iOS and
vendored packages), `tests/legendary/`, `.github/workflows/cpython-ios.yml`. Written against
Legendary tag 0.21.1 (`external/legendary`, `pyproject.toml` version) and CPython v3.14.10
(`Include/patchlevel.h`). All `file:line` refer to those trees. Neither tree was modified.

No real login was performed, no Epic server was contacted, no token file was read, and the game
directory was not touched. The only hosts contacted: PyPI (wheel downloads, hashes), Debian mirrors
(Swift toolchain), developer.apple.com (symbol existence), `legendary.gl` (one `HEAD` on
`/epiclogin` to read its redirect target; not an Epic server).

## 1. Legendary facts (with citations)

| Question | Answer |
|---|---|
| Login URL | `EPCAPI.get_auth_url()` `legendary/api/egs.py:62-65`: `https://www.epicgames.com/id/login?redirectUrl=` + URL-quoted `https://www.epicgames.com/id/api/redirect?clientId=<_user_basic>&responseType=code`. This method is **not called anywhere in 0.21.1**. The CLI sends users to `https://legendary.gl/epiclogin` (`cli.py:177`); `curl -I` on 2026-10-29 shows a `302` whose `location:` equals `get_auth_url()` byte for byte (pinned in `test_bridge.py::LEGENDARY_GL_REDIRECT`). The bridge uses `get_auth_url()` so there is no third-party hop. `clientId=34a02cf8f4414e29b15921876da36f9a` is the same client whose secret `start_session` uses (`egs.py:24-25, 100-110`), so the code is exchangeable. |
| Where the code appears | After login Epic redirects to `.../id/api/redirect?clientId=...&responseType=code`; the body is JSON with `authorizationCode` (README.md:145; `cli.py:180-185`: accepts the whole JSON object or the bare/quoted code). Only that field is documented by Legendary; the JSON also has other keys (tolerated). |
| Exchange of the code | `LegendaryCore.auth_code(code) -> bool` (`core.py:157-167`) -> `EPCAPI.start_session(authorization_code=code)` (`egs.py:100-147`, POST `/account/api/oauth/token`, `grant_type=authorization_code`); tokens persisted to `user.json` through `lgd.userdata_lock` (`lfs/lgndry.py:148-157`). Display name: `core.lgd.userdata["displayName"]` (`cli.py:206`). Related: `auth_ex_token` (`core.py:169`, exchange code) is what the pywebview flow uses (`utils/webview_login.py:85-104`); the spec asks for the authorization-code route. |
| Import of an existing install | CLI `import_game` (`cli.py:1348-1438`): `core.lgd.lock_installed()`, `core.login()`, `core.get_game(app, update_meta=True)` (`core.py:515-518`; refreshes assets and catalog metadata, which is what caches `extraLaunchOption_*`), `core.import_game(game, path)` (`core.py:1890-1958`; uses `.egstore` manifest if present, else downloads the CDN manifest), checks, `core.install_game(igame)` (`core.py:1824`, writes `installed.json`). `legendary move --skip-move` repoints `install_path` (`cli.py:2667-2668`). |
| App name | `Sugar` (DECISIONS D-001). `import_game` records `executable = manifest.meta.launch_exe` (= `Binaries/Win64/Launcher.exe`, D-002) (`core.py:1953`). |
| Launch parameters | `LegendaryCore.get_launch_parameters(app_name, offline=False, user=None, extra_args=None, wine_bin=None, wine_pfx=None, language=None, wrapper=None, disable_wine=False, executable_override=None, crossover_app=None, crossover_bottle=None, addon_app_name=None) -> LaunchParameters` (`core.py:830-936`). `LaunchParameters` is a dataclass (`models/game.py:295-314`) with `game_parameters, game_executable, game_directory, egl_parameters, launch_command, working_directory, user_parameters, environment, pre_launch_command, pre_launch_wait`. It does **not** log in: the CLI calls `core.login()` first (`cli.py:632-636`). Fresh exchange code: `egs.get_game_token()['code']` (`core.py:889-892`, `egs.py:150-153`), put in `-AUTH_PASSWORD=`. Auth args (`core.py:899-925`): `-AUTH_LOGIN=unused -AUTH_PASSWORD=<code> -AUTH_TYPE=exchangecode -epicapp=<app> -epicenv=Prod [-epicovt=<path>] -EpicPortal -epicusername= -epicuserid= -epiclocale= -epicsandboxid= [-epicdeploymentid=]`. CLI argv order: launch_command, exe, `game_parameters`, `user_parameters`, `egl_parameters` (`cli.py:695-701`). |
| `extraLaunchOption*` | Legendary does **not** apply them. It only *displays* them in `legendary info`: loop over `customAttributes.extraLaunchOption_{i:03d}_Name/_Args` `.value` (`cli.py:1736-1745`). They live in the cached catalog metadata `metadata/<app>.json` under `metadata.customAttributes` (written by `lfs/lgndry.py:267-272` from `core.py:595`). A separate attribute, `AdditionalCommandLine`, **is** applied automatically (`models/game.py:157-161`, `core.py:882-886`). The bridge reads the option the same way `info` does and feeds its args through `extra_args`, i.e. the equivalent of `legendary launch Sugar -noeac`. |
| Config directory | Env `LEGENDARY_CONFIG_PATH` (`lfs/lgndry.py:26-27`), else `$XDG_CONFIG_HOME/legendary`, else `~/.config/legendary`. Read in `LGDLFS.__init__` only, so it must be set before `LegendaryCore()` is created. |
| Offline | `legendary launch --offline` or per-game `[<app>] offline = true` (`cli.py:632`, `core.py:1359`). Skips login and the update check; `get_launch_parameters(offline=True)` skips `get_game_token` (so `-AUTH_PASSWORD=` is empty), warns if `can_run_offline` is false (`core.py:890-894`), but still reads `lgd.userdata['account_id'/'displayName']` (`core.py:896-897`), so a previous login must exist on disk. |
| Other network | `core.login()` -> `check_for_updates()` calls `api.legendary.gl/v1/version.json` (`core.py:231-236, 293-305`, `api/lgd.py:20-24`) unless `[Legendary] disable_update_check = true`; failures are caught. Its payload can update the client id/secret (`apply_lgd_config`, `core.py:307-325`). The bridge leaves this at Legendary's default; see "Proposed" below. |

## 2. Bridge (`ios/legendary/bridge.py`)

API: `configure(config_dir, ca_bundle=None, game_dir=None)`, `login_url()`, `complete_login(code) -> name`,
`register_game(install_dir, app_name="Sugar")`, `launch_args(app_name="Sugar", offline=False) -> dict`,
`redacted_argv(argv)`, `status()`. Errors: `BridgeError` subclasses with stable `.code` and
user-presentable `str()`.

`launch_args` (D-005):
1. `core.login()` (session refresh) unless offline.
2. Reads the option whose **name** is `Launch without Anti-Cheat` from cached metadata and takes its
   `Args` value (tokenised with `shlex.split(posix=False)` as Legendary does for metadata params,
   `core.py:884`). Missing metadata or option: `LaunchOptionMissingError`, raised **before** any exchange
   code is requested; nothing is returned (no fallback).
3. `core.get_launch_parameters(..., extra_args=<option args>, disable_wine=True)`; argv = game + user + egl params
   in the CLI's order.
4. Applies the Launcher.exe row: `LAUNCHER_SWITCH_TABLE = {"-noeac": "RocketLeague.exe"}`, case-insensitive,
   removes the token, picks `RocketLeague.exe` in the Launcher's directory (`Binaries/Win64/`). The table is
   the one hard-coded fact (D-005: recon of Launcher.exe); the **value** `-noeac` comes from Epic's
   metadata, and an option whose args hit no table row raises `LaunchOptionUnmappedError`. The `-eac` row is
   deliberately not mirrored.
5. Returns `exe` (relative), `argv`, `cwd`, plus `game_dir`, `wine_exe`/`wine_cwd`
   (`C:\Games\RocketLeague\Binaries\Win64\...`), `env`, `offline`, `can_run_offline`. `argv` holds a one-time
   exchange code: never log it (`redacted_argv`), launch promptly.

Not done on purpose: the "is the game up to date" check of `legendary launch` (`cli.py:638-651`);
Wine/CrossOver command lines (`disable_wine=True`, so `launch_command` stays empty); `-epicovt` is produced by Legendary
when `requires_ot` is set (`core.py:906-911`) and arrives in `argv` unchanged.

`register_game` is idempotent: an existing record only has its path repointed (container UUIDs of sideloaded
apps can change); a new record needs network and a prior `complete_login`.

### pycryptodomex placeholder
`legendary.core` imports `Cryptodome.Cipher.AES` at module level (`models/chunk.py:8`, `models/manifest.py:13`),
but only calls `AES.new` to decrypt encrypted chunks/manifests (`chunk.py:45`, `manifest.py:105`). pycryptodomex is
**not pure Python** and has no iOS wheel (PyPI JSON for 3.24.1: no `ios` wheel; `pip download --platform
ios_13_0_arm64_iphoneos` finds none). It also loads its C code with `cffi`/`ctypes` by file path
(`Cryptodome/Util/_raw_api.py:297-320`), which cannot find a module that iOS packaging has turned into a `.fwork`
marker plus a binary under `Frameworks/` (Doc/using/ios.rst:70-115). If the real package cannot be imported the
bridge installs a placeholder whose `AES.new` raises "cannot decrypt"; it implements no cryptography. It matters
only if Epic serves an *encrypted* manifest or chunk, which `register_game` could meet when no `.egstore`
manifest exists. Real fix: build pycryptodomex for iOS and patch its loader to resolve through the `.fwork` file.

## 3. TLS: what actually takes effect

Legendary uses only `requests` sessions: `EPCAPI.session`/`unauth_session` (`api/egs.py:42-46`), `LGDAPI.session`
(`api/lgd.py:16`), and ad hoc `session()` objects (`core.py:129`). Findings on requests 2.34.2 (tests in
`test_tls.py`, using a local HTTPS server with a throw-away CA):

* `REQUESTS_CA_BUNDLE` (or `CURL_CA_BUNDLE`) is read **per request** in `Session.merge_environment_settings`
  (`requests/sessions.py:853-859`), so it also covers sessions Legendary creates later. It takes precedence over
  `session.verify` when the request passes `verify=None/True`.
* `session.verify = path` works for that one session only (`adapters.py:307-340` `cert_verify`).
* Without either, `requests` uses `DEFAULT_CA_BUNDLE_PATH = certs.where()` = `certifi.where()` (`utils.py:82`), i.e.
  certifi is already the default when it is importable; the bundle must live on disk (it does in `app_packages`).
* `SSL_CERT_FILE` alone does **not** make `requests` trust a CA (test `test_ssl_cert_file_alone_does_not_affect_requests`).
  It is still exported for any stdlib `ssl.create_default_context()` user. GnuTLS/Wine's CA store is a separate
  matter (spec section 5).

`configure()` sets `REQUESTS_CA_BUNDLE`, `SSL_CERT_FILE` and `verify` on Legendary's three long-lived sessions;
default bundle `certifi.where()`.

## 4. CPython for iOS (`tools/cpython/`)

Official procedure in this tag: `external/cpython/Apple/iOS/README.md:52-76` -> `python Apple build iOS`
(`Apple/__main__.py:751-782`): build Python, download iOS binary deps (`BZip2-1.0.8-2, libFFI-3.4.7-2,
OpenSSL-3.5.9-1, XZ-5.6.4-2, mpdecimal-4.0.0-2, zstd-1.5.7-1` from `beeware/cpython-apple-source-deps`,
`__main__.py:327-341`), build hosts `arm64-apple-ios`, `arm64-apple-ios-simulator`, `x86_64-apple-ios-simulator`
(`__main__.py:69-77`), merge into `cross-build/iOS/Python.xcframework` with slices `ios-arm64` and
`ios-arm64_x86_64-simulator`, tarball in `cross-build/dist/`. Differences from older docs: the build entry point
is `Apple/`, not `iOS/` (`iOS/` only holds a stub `Resources`); the deployment target default is iOS 13.0 unless
the host triple carries a version (`README.md:212-216`); `package` runs `Apple/testbed` by a path relative to the
cwd (`__main__.py:721-730`), hence `build_ios.sh` does `cd external/cpython` and passes `--cross-build-dir` to keep
build products out of the submodule.

App integration (not written here; the app shell owner needs it): `Doc/using/ios.rst:163-250` and
`Python.xcframework/build/utils.sh` (`install_python Python.xcframework app app_packages` in a Run Script phase
converts every `.so` to a signed framework with a `.fwork` marker); interpreter config: UTF-8 mode on, buffered
stdio off, no bytecode writing, `PYTHONHOME` = `<bundle>/python`, `PYTHONPATH` = `python/lib/python3.14`,
`.../lib-dynload`, `app` and `app_packages` (`ios.rst:231-250`). Legendary's `__version__` comes from
`importlib.metadata.version('legendary-gl')` (`legendary/__init__.py:4`), so the wheel's `.dist-info` must stay in
`app_packages` (it does; `vendor_deps.sh` unpacks wheels).

`vendor_deps.sh`: Legendary's `[project].dependencies` are `requests`, `filelock`, `pycryptodomex`
(`pyproject.toml`); the rest of the closure comes from `uv.lock`. Pins and sha256 in
`tools/cpython/requirements-ios.txt` are the `py3-none-any` entries of that lockfile; `pip download
--require-hashes --only-binary=:all:` enforces them (a tampered hash fails, shown in the session). The Legendary
wheel is built from the submodule (reproducible: wheel sha256 `c8ee1ed3...` equal across two builds).
The script refuses to finish if any `.so/.dylib/.pyd/.a` is present.

## 5. WKWebView login (`ios/Auth/`)

`LoginView` (SwiftUI `UIViewRepresentable`) loads `login_url()`, and on `didFinish` of
`https://www.epicgames.com/id/api/redirect` reads `document.body.innerText`, parses the JSON and hands
`authorizationCode` to `onCode` once. Code never logged; non-persistent `WKWebsiteDataStore`; `NSURLErrorCancelled`
ignored; `window.open` stays in the same view (identity-provider popups). The URL/JSON logic is in
`AuthCodeExtractor.swift` (Foundation only) so it compiles and is tested on Linux. Apple symbols were each
confirmed to exist in Apple's documentation data (HTTP 200 on `developer.apple.com/tutorials/data/documentation/
webkit/...`; a made-up symbol returns 404). Not verified: behaviour inside a real WKWebView (captcha, SSO
providers, the JSON being exposed via `innerText`), and a type-check against the iOS SDK.

## 6. Verification status

Run on Debian 13 (x86-64), Python 3.13.5 venv, legendary-gl 0.21.1 installed from the submodule path:
* `pytest tests/legendary`: 38 passed without a Swift compiler, 40 passed with one (2 Swift tests otherwise skipped).
  Mutation check: breaking CA configuration, switch removal, exe mapping, or the option-before-token ordering each
  makes 3-5 tests fail; restored code passes.
* `tools/cpython/vendor_deps.sh`: ran end to end (7 wheels, 137 CA certificates, no compiled modules); the bridge
  imports and builds the login URL from `app_packages` alone under `python3 -I` with no pycryptodomex.
* `shellcheck` clean; `actionlint 1.7.12` clean on `cpython-ios.yml`; `tools/cpython/build_ios.sh` on Linux exits 1 with a
  clear message.
* Swift 6.0.3 (Debian `swiftlang`, unpacked with `dpkg -x`, no install): `swiftc -parse` passes for both Swift
  files; `AuthCodeExtractor.swift` compiled and its 17 checks pass.

Not verified (needs macOS/Xcode/device): `build_ios.sh` and the workflow run; the `lipo`/`vtool` assertions in
`build_ios.sh` (written from tool documentation, never executed); that `import legendary.core` works on iOS
(it pulls in `multiprocessing` and `multiprocessing.shared_memory` at import time, `downloader/mp/manager.py:9-11`,
while CPython documents multiprocessing as unusable on iOS, `Doc/library/intro.rst:132-137`; importing may still
succeed, spawning would not and the bridge never spawns); `LoginView` in a WebView; any real Epic response
(`register_game`, token exchange, metadata contents including whether `extraLaunchOption_001` is really cached
for `Sugar` after `get_game(update_meta=True)`).

Suggested next verification on a Mac: clone the CPython testbed with `--app ios/legendary` and the
`app_packages` folder (`Doc/using/ios.rst:296-331`, "Testing a Python package") and run
`import bridge; bridge.configure(...); bridge.login_url()` in the Simulator.

## 7. Proposed DECISIONS / RISKS (for the owner to merge; not edited here)

**D-0xx: Legendary embedded as the unmodified 0.21.1 wheel behind `ios/legendary/bridge.py`.**
Login uses `EPCAPI.get_auth_url()` (WKWebView, authorization-code route); the app never reimplements Epic
auth. Launch args come from `LegendaryCore.get_launch_parameters`; the bridge only (a) selects Epic's published
"Launch without Anti-Cheat" option from cached metadata, (b) applies D-005's `-noeac` row (exe choice, token
dropped), (c) returns argv for in-process start. No metadata option, no launch (no fallback). Pure-Python deps
are pinned with hashes from Legendary's own `uv.lock`; `certifi` is the CA bundle (`REQUESTS_CA_BUNDLE`).

**D-0xx: `legendary.gl` update check stays enabled** (Legendary's default; supplies current EGS client parameters
and is failure-tolerant). If the owner prefers no contact with a third party, set
`[Legendary] disable_update_check = true` in `config.ini`; expect breakage if Epic rotates client parameters.

**R-0xx: pycryptodomex has no iOS build and its loader cannot find `.fwork` modules.** Impact: encrypted manifests
or chunks cannot be decrypted; the bridge's placeholder raises a clear error. Auth, import with an unencrypted or
`.egstore` manifest, and launch arguments do not depend on it. Mitigation: cross-build it (cibuildwheel's iOS
platform or CPython's cross environment + `Apple/iOS/Resources/bin` compilers) and patch
`load_pycryptodome_raw_lib` to resolve via `.fwork`; or import the game from a copy that has the `.egstore` folder.

**R-0xx: `import legendary.core` on iOS is unproven** (`multiprocessing`/`shared_memory`/`_posixshmem` imported at module
level). Mitigation if it fails: stub those imports in the bridge (the app never runs Legendary's downloader).

**R-0xx: Epic web login inside WKWebView** may be blocked by captcha/bot checks or an identity-provider flow that needs
a real browser. Mitigation: `login_url()` also works in Safari via `ASWebAuthenticationSession`/manual paste:
`complete_login()` accepts the bare code or the pasted JSON (`cli.py:178-186` behaviour).

**R-0xx: one-time exchange code in argv.** `-AUTH_PASSWORD=` is valid for a short time and must be fetched right before
start; the app must not log argv (use `redacted_argv`) nor persist it.

**R-0xx: `locale`.** On iOS `LANG` is usually unset, so Legendary logs "Could not determine locale" and uses `en-US`
(`core.py:104-115`); set `[Legendary] locale = <xx-YY>` in `config.ini` if another `-epiclocale` is wanted.
