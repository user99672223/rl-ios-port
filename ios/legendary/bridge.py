"""Legendary bridge for the iOS app (spec 6.7, DECISIONS D-001 / D-005).

The Swift app calls this module through embedded CPython.  It is a thin layer over
Legendary 0.21.1's own ``LegendaryCore``; nothing here talks to Epic except through
Legendary's normal code paths, with the owner's own account.  Nothing here bypasses,
emulates, disables or spoofs EAC, DRM or ownership checks.

Public API (all arguments and results are plain str / list / dict / None)::

    configure(config_dir, ca_bundle=None, game_dir=None)   # call once, first
    login_url() -> str
    complete_login(authorization_code) -> str               # account display name
    register_game(install_dir, app_name="Sugar") -> None
    launch_args(app_name="Sugar", offline=False) -> dict    # see its docstring
    redacted_argv(argv) -> list[str]                        # safe to log
    status(app_name="Sugar") -> dict                        # offline, no network

Errors are ``BridgeError`` subclasses.  ``str(exc)`` is written to be shown to the user
as is and ``exc.code`` is a stable machine-readable string for the Swift side.

Secrets: the authorization code, the exchange code in ``-AUTH_PASSWORD=...`` and the
tokens in ``user.json`` must never be logged.  This module logs none of them; use
``redacted_argv`` if you want to log an argv.
"""
from __future__ import annotations

import json
import logging
import os
import posixpath
import shlex
import sys
import threading
import types

log = logging.getLogger("rlport.legendary")

#: Legendary app name of Rocket League (D-001; Epic catalog / Legendary metadata).
APP_NAME = "Sugar"

#: Name of Epic's published extra launch option (catalog attribute
#: ``extraLaunchOption_NNN_Name``).  Its *Args* value is read from metadata, never hard-coded.
LAUNCH_OPTION_NAME = "Launch without Anti-Cheat"

#: Mirror of the ``-noeac`` row of the game's ``Launcher.exe`` switch table (D-005, recon).
#: Key: switch (matched case-insensitively).  Value: executable, in the Launcher's own
#: directory, that Launcher.exe starts for it.  Only this one row is mirrored; the
#: ``-eac`` row (RocketLeague_EAC.exe) is deliberately absent.
LAUNCHER_SWITCH_TABLE = {"-noeac": "RocketLeague.exe"}

#: Where Wine sees the game directory (spec 6.7 task text).
WINE_GAME_ROOT = "C:\\Games\\RocketLeague"

#: Stable link Legendary's own CLI sends users to for manual login (cli.py:177).  It 302s
#: to the same URL ``login_url()`` returns (verified with ``curl -I``; see docs/notes/auth.md).
LEGENDARY_MANUAL_LOGIN_URL = "https://legendary.gl/epiclogin"


# --------------------------------------------------------------------------- errors
class BridgeError(Exception):
    """Base class.  ``code`` is stable, the message is user-presentable."""

    code = "error"


class NotConfiguredError(BridgeError):
    code = "not_configured"


class NotLoggedInError(BridgeError):
    code = "not_logged_in"


class LoginError(BridgeError):
    code = "login_failed"


class GameNotOnAccountError(BridgeError):
    code = "game_not_on_account"


class GameFilesError(BridgeError):
    code = "game_files"


class GameNotInstalledError(BridgeError):
    code = "game_not_registered"


class LaunchOptionMissingError(BridgeError):
    """Epic's cached metadata has no 'Launch without Anti-Cheat' option (D-001)."""

    code = "launch_option_missing"


class LaunchOptionUnmappedError(BridgeError):
    """The metadata option exists but none of its switches is in the Launcher table."""

    code = "launch_option_unmapped"


# --------------------------------------------------------------------------- state
_lock = threading.RLock()
_state: dict = {"config_dir": None, "ca_bundle": None, "game_dir": None, "core": None}


def _ensure_cryptodome() -> None:
    """Make ``import legendary.core`` possible when pycryptodomex is not built for iOS.

    Legendary imports ``Cryptodome.Cipher.AES`` at module level (models/chunk.py:8,
    models/manifest.py:13) but only calls ``AES.new`` to decrypt encrypted chunks
    (chunk.py:45) and encrypted manifests (manifest.py:105).  Auth, import of an
    unencrypted manifest and launch-parameter building never reach it.  pycryptodomex
    ships no iOS wheel and loads its C code with cffi/ctypes, so until it is built for
    iOS this placeholder lets the module import.  Using it to decrypt raises a clear
    error; it implements no cryptography.  If the real package is importable it is used.
    """
    try:
        import Cryptodome.Cipher.AES  # noqa: F401
        return
    except ImportError:
        pass
    log.warning("pycryptodomex is not importable; encrypted manifests/chunks unsupported")

    def _unavailable(*_a, **_k):
        raise RuntimeError("pycryptodomex is not available in this build; "
                           "cannot decrypt encrypted Epic manifests or chunks")

    aes = types.ModuleType("Cryptodome.Cipher.AES")
    aes.MODE_GCM = 0
    aes.new = _unavailable
    cipher = types.ModuleType("Cryptodome.Cipher")
    cipher.AES = aes
    root = types.ModuleType("Cryptodome")
    root.Cipher = cipher
    sys.modules.update({"Cryptodome": root, "Cryptodome.Cipher": cipher,
                        "Cryptodome.Cipher.AES": aes})


def configure(config_dir: str, ca_bundle: str | None = None, game_dir: str | None = None) -> None:
    """Select Legendary's config directory, the CA bundle and the game directory.

    * ``config_dir``: exported as ``LEGENDARY_CONFIG_PATH``, which ``LGDLFS.__init__``
      reads (legendary/lfs/lgndry.py:26-27).  It is read once at ``LegendaryCore()``
      construction, so this must run before the first call below.
    * ``ca_bundle``: path to a PEM CA file (default: ``certifi.where()``).  Exported as
      ``REQUESTS_CA_BUNDLE`` (requests/sessions.py ``merge_environment_settings``, which
      applies it to every ``requests`` session Legendary creates, also later ones) and
      ``SSL_CERT_FILE`` (stdlib ``ssl`` default context, for completeness), and set as
      ``verify`` on Legendary's long-lived sessions.
    * ``game_dir``: where the game lives now.  If Legendary's install record points
      elsewhere (iOS container paths can change on reinstall) ``register_game`` and
      ``launch_args`` repoint it.
    """
    with _lock:
        if not config_dir:
            raise NotConfiguredError("config_dir is required")
        if ca_bundle is None:
            import certifi
            ca_bundle = certifi.where()
        if not os.path.isfile(ca_bundle):
            raise NotConfiguredError(f"CA bundle not found: {ca_bundle}")
        os.makedirs(config_dir, exist_ok=True)
        os.environ["LEGENDARY_CONFIG_PATH"] = config_dir
        os.environ["REQUESTS_CA_BUNDLE"] = ca_bundle
        os.environ["SSL_CERT_FILE"] = ca_bundle
        _state.update(config_dir=config_dir, ca_bundle=ca_bundle,
                      game_dir=os.path.abspath(game_dir) if game_dir else None, core=None)


def _core():
    """The (lazily created) LegendaryCore, with TLS pointed at the configured bundle."""
    with _lock:
        if _state["core"] is not None:
            return _state["core"]
        if not _state["config_dir"]:
            raise NotConfiguredError("call configure() first")
        _ensure_cryptodome()
        from legendary.core import LegendaryCore  # after env + crypto fallback are set
        core = LegendaryCore()
        ca = _state["ca_bundle"]
        for s in (core.egs.session, core.egs.unauth_session, core.lgdapi.session):
            s.verify = ca
        # Apply cached Legendary-published EGS parameters (client id etc., core.py:307);
        # no network.  Done here so login_url() and complete_login() use the same client.
        core.apply_lgd_config()
        _state["core"] = core
        return core


# --------------------------------------------------------------------------- login
def login_url() -> str:
    """URL to open in a web view to obtain an authorization code.

    ``EPCAPI.get_auth_url()`` (legendary/api/egs.py:62-65).  After login Epic redirects to
    ``https://www.epicgames.com/id/api/redirect?clientId=...&responseType=code`` whose body
    is JSON containing ``authorizationCode`` (README.md:145; cli.py:180-185).
    """
    return _core().egs.get_auth_url()


def _extract_code(value: str) -> str:
    """Accept a bare code, a quoted code, or the pasted JSON (cli.py:178-186)."""
    code = (value or "").strip()
    if code.startswith("{"):
        try:
            code = str(json.loads(code).get("authorizationCode") or "")
        except ValueError:
            code = ""
    return code.strip().strip('"')


def complete_login(authorization_code: str) -> str:
    """Exchange an authorization code for a session; return the account display name.

    ``LegendaryCore.auth_code`` (core.py:157-167) -> ``EPCAPI.start_session(
    authorization_code=...)`` (egs.py:100-147), which stores tokens in ``user.json`` in
    the config directory.  The code is never logged.
    """
    code = _extract_code(authorization_code)
    if not code:
        raise LoginError("No authorization code was provided.")
    with _lock:
        core = _core()
        if not core.auth_code(code):
            raise LoginError("Epic rejected the login. The code may have expired; "
                             "please sign in again.")
        name = (core.lgd.userdata or {}).get("displayName")
        if not name:
            raise LoginError("Login succeeded but Epic returned no account name.")
        return str(name)


def _require_login(core) -> None:
    """Refresh the session like ``legendary launch`` / ``import`` do (cli.py:635, 1366)."""
    try:
        ok = core.login()
    except ValueError as e:  # LegendaryCore._login: 'No saved credentials'
        raise NotLoggedInError("Not signed in to Epic. Please sign in first.") from e
    if not ok:
        raise LoginError("Could not refresh the Epic session (offline, or the saved login "
                         "expired). Check the connection or sign in again.")


# --------------------------------------------------------------------------- registration
def register_game(install_dir: str, app_name: str = APP_NAME) -> None:
    """Register an existing install with Legendary (the ``legendary import`` flow).

    Follows ``import_game`` in cli.py:1348-1438 without the prompts:
    ``core.login()``, ``core.get_game(app, update_meta=True)`` (downloads and caches the
    catalog metadata, including ``extraLaunchOption_*``), ``core.import_game`` (core.py:1890;
    uses ``.egstore`` if present, otherwise downloads the manifest), sanity checks, then
    ``core.install_game``.  Needs network and a prior ``complete_login``.  Idempotent: if
    the game is already registered, only the install path is repointed (as ``legendary
    move --skip-move``, cli.py:2667-2668).
    """
    from legendary.lfs.wine_helpers import case_insensitive_file_search

    path = os.path.abspath(install_dir)
    if not os.path.isdir(path):
        raise GameFilesError(f"Game folder not found: {path}")
    with _lock:
        core = _core()
        if not core.lgd.lock_installed():
            raise GameFilesError("Another Legendary operation holds the install lock.")

        igame = core.get_installed_game(app_name, skip_sync=True)
        if igame is not None:
            if igame.install_path != path:
                log.info("repointing %s install path", app_name)
                igame.install_path = path
                core.install_game(igame)
            return

        _require_login(core)
        game = core.get_game(app_name, update_meta=True)
        if not game:
            raise GameNotOnAccountError(f"Epic reports no game '{app_name}' on this account.")

        manifest, igame = core.import_game(game, path)
        elements = manifest.file_manifest_list.elements
        found = sum(os.path.exists(os.path.join(path, f.filename)) for f in elements)
        if not found:
            raise GameFilesError("None of the game's files were found in the game folder; "
                                 "check that the game was copied to the right place.")
        exe_path = case_insensitive_file_search(os.path.join(path, manifest.meta.launch_exe.lstrip("/")))
        if not os.path.exists(exe_path):
            raise GameFilesError(f"The game's launcher executable is missing: {manifest.meta.launch_exe}")
        if found / len(elements) < 0.95:
            log.warning("some game files are missing (%d of %d present)", found, len(elements))
        core.install_game(igame)


# --------------------------------------------------------------------------- launch
def _launch_options(metadata: dict) -> list[tuple[str, str]]:
    """(name, args) pairs, same loop as ``legendary info`` (cli.py:1738-1745)."""
    attrs = (metadata or {}).get("customAttributes", {})
    out = []
    i = 1
    while f"extraLaunchOption_{i:03d}_Name" in attrs:
        name = attrs[f"extraLaunchOption_{i:03d}_Name"].get("value", "")
        args = attrs.get(f"extraLaunchOption_{i:03d}_Args", {}).get("value", "")
        out.append((name, args))
        i += 1
    return out


def _pick_launch_option(metadata: dict) -> tuple[str, list[str]]:
    for name, args in _launch_options(metadata):
        if name == LAUNCH_OPTION_NAME:
            # Same tokenisation Legendary uses for metadata launch parameters (core.py:884).
            return name, shlex.split(args.strip(), posix=False)
    raise LaunchOptionMissingError(
        f"Epic's metadata for this game has no '{LAUNCH_OPTION_NAME}' launch option, so it "
        "cannot be started on this device. Refresh the game data while online and try again.")


def _apply_launcher_switch(argv: list[str]) -> tuple[str, list[str]]:
    """Reproduce Launcher.exe's switch handling (D-005): pick the exe, drop the switch.

    Matching is case-insensitive; every matching token is removed; all other arguments
    keep their order.  Returns (executable file name, remaining argv).
    """
    exe = None
    rest = []
    for tok in argv:
        mapped = LAUNCHER_SWITCH_TABLE.get(tok.lower())
        if mapped is not None:
            exe = exe or mapped
        else:
            rest.append(tok)
    if exe is None:
        raise LaunchOptionUnmappedError(
            f"The launch option from Epic ({' '.join(argv)!r}) is not one this app knows how "
            "to apply.")
    return exe, rest


def launch_args(app_name: str = APP_NAME, offline: bool = False) -> dict:
    """Build the launch command for ``Binaries/Win64/RocketLeague.exe`` (D-005).

    Steps, all through Legendary:

    1. ``core.login()`` refreshes the session (unless offline; cli.py:632-636).
    2. The 'Launch without Anti-Cheat' option's ``Args`` are read from the cached game
       metadata (``customAttributes.extraLaunchOption_NNN_*``), no hard-coded value.
       Missing -> ``LaunchOptionMissingError`` and nothing is returned.
    3. ``core.get_launch_parameters(app, offline, extra_args=<option args>,
       disable_wine=True)`` (core.py:830-936) fetches a fresh exchange code
       (``egs.get_game_token``) and returns ``LaunchParameters``.
    4. Argument order is the CLI's (cli.py:699-701): game params, user params (here the
       option args), then Legendary's auth params.  The Launcher.exe ``-noeac`` row is then
       applied: executable becomes RocketLeague.exe in the Launcher's directory and the
       switch is removed.

    Returns ``{"exe", "argv", "cwd", ...}``: ``exe`` is relative to ``game_dir``;
    ``argv`` excludes the executable; ``cwd`` is the host working directory.  Also
    ``game_dir`` (host), ``wine_exe`` / ``wine_cwd`` (Windows paths under
    ``WINE_GAME_ROOT``), ``env`` (Legendary's per-game env overrides), ``offline``,
    ``can_run_offline``.  ``argv`` contains a one-time exchange code: do not log it
    (use ``redacted_argv``) and launch promptly.

    ``offline=True`` mirrors ``legendary launch --offline``: no network, empty auth
    password; still needs a prior login on disk for the account name and id.  The
    per-game ``offline`` config key also forces it (cli.py:632).  The online "is the game
    up to date" check (cli.py:638-651) is not performed.
    """
    with _lock:
        core = _core()
        igame = core.get_installed_game(app_name, skip_sync=True)
        if igame is None:
            raise GameNotInstalledError("The game is not registered yet. Run game setup first.")
        game_dir = _state["game_dir"]
        if game_dir and igame.install_path != game_dir and os.path.isdir(game_dir):
            igame.install_path = game_dir
            core.install_game(igame)
        if not os.path.isdir(igame.install_path):
            raise GameFilesError(f"Game folder not found: {igame.install_path}")

        game = core.lgd.get_game_meta(app_name)
        if game is None:
            raise LaunchOptionMissingError(
                "No cached Epic metadata for this game. Refresh the game data while online.")
        _, option_args = _pick_launch_option(game.metadata)  # before any token is requested

        offline = bool(offline) or core.is_offline_game(app_name)
        if not offline:
            _require_login(core)
        elif core.lgd.userdata is None:
            raise NotLoggedInError("Offline launch still needs a previous Epic sign-in.")

        params = core.get_launch_parameters(app_name=app_name, offline=offline,
                                            extra_args=list(option_args), disable_wine=True)

        full = [*params.game_parameters, *params.user_parameters, *params.egl_parameters]
        exe_name, argv = _apply_launcher_switch(full)

        launcher_rel = params.game_executable.replace("\\", "/").lstrip("/")
        exe_rel = posixpath.join(posixpath.dirname(launcher_rel), exe_name)
        exe_host = os.path.join(igame.install_path, exe_rel)
        from legendary.lfs.wine_helpers import case_insensitive_file_search
        if not os.path.exists(case_insensitive_file_search(exe_host)):
            raise GameFilesError(f"{exe_rel} is missing from the game folder.")

        rel_dir = posixpath.dirname(exe_rel)
        wine_cwd = WINE_GAME_ROOT + ("\\" + rel_dir.replace("/", "\\") if rel_dir else "")
        return {
            "app_name": app_name,
            "exe": exe_rel,
            "argv": argv,
            "cwd": os.path.dirname(exe_host),
            "game_dir": igame.install_path,
            "wine_exe": wine_cwd + "\\" + exe_name,
            "wine_cwd": wine_cwd,
            "env": dict(params.environment),
            "offline": offline,
            "can_run_offline": bool(igame.can_run_offline),
        }


_SECRET_PREFIXES = ("-auth_password=", "-auth_login=")


def redacted_argv(argv: list[str]) -> list[str]:
    """Copy of ``argv`` with the exchange code replaced, safe for logs."""
    return [(a.split("=", 1)[0] + "=<redacted>") if a.lower().startswith(_SECRET_PREFIXES) else a
            for a in argv]


def status(app_name: str = APP_NAME) -> dict:
    """Local, offline summary for the UI: login, registration, launch-option availability."""
    with _lock:
        core = _core()
        user = core.lgd.userdata
        igame = core.get_installed_game(app_name, skip_sync=True)
        game = core.lgd.get_game_meta(app_name)
        has_opt = False
        if game is not None:
            has_opt = any(n == LAUNCH_OPTION_NAME for n, _ in _launch_options(game.metadata))
        return {
            "logged_in": bool(user),
            "display_name": (user or {}).get("displayName"),
            "registered": igame is not None,
            "install_path": igame.install_path if igame else None,
            "metadata_cached": game is not None,
            "launch_option_available": has_opt,
        }
