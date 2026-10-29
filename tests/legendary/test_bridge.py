"""Bridge behaviour against Legendary 0.21.1 with all network calls mocked."""
from __future__ import annotations

import json
import logging
import os
import subprocess
import sys
import textwrap
from urllib.parse import parse_qs, urlparse

import pytest

from conftest import (ACCOUNT_ID, APP, AUTH_CODE, DISPLAY_NAME, EXCHANGE_CODE, MANIFEST,
                      OPTION_ARGS, OPTION_NAME, json_manifest)

# Location header returned by `curl -sI https://legendary.gl/epiclogin` (Legendary's own
# manual-login link, cli.py:177), captured 2026-10-29.  Must equal EPCAPI.get_auth_url().
LEGENDARY_GL_REDIRECT = (
    "https://www.epicgames.com/id/login?redirectUrl="
    "https%3A//www.epicgames.com/id/api/redirect%3FclientId%3D34a02cf8f4414e29b15921876da36f9a"
    "%26responseType%3Dcode")


# ------------------------------------------------------------------------------ login
def test_login_url_matches_legendary_source(bridge, cfg):
    from legendary.api.egs import EPCAPI

    bridge.configure(str(cfg))
    url = bridge.login_url()

    assert url == EPCAPI().get_auth_url()          # egs.py:62-65, real class
    assert url == LEGENDARY_GL_REDIRECT            # what legendary.gl/epiclogin redirects to
    q = urlparse(url)
    assert (q.scheme, q.netloc, q.path) == ("https", "www.epicgames.com", "/id/login")
    redirect = urlparse(parse_qs(q.query)["redirectUrl"][0])
    assert (redirect.netloc, redirect.path) == ("www.epicgames.com", "/id/api/redirect")
    assert parse_qs(redirect.query) == {"clientId": [EPCAPI._user_basic], "responseType": ["code"]}


def test_configure_selects_legendary_config_dir(bridge, cfg):
    bridge.configure(str(cfg))
    core = bridge._core()
    assert os.environ["LEGENDARY_CONFIG_PATH"] == str(cfg)   # lgndry.py:26
    assert core.lgd.path == str(cfg)
    for sub in ("manifests", "metadata", "tmp"):
        assert (cfg / sub).is_dir()


def test_complete_login_exchanges_code_and_returns_name(bridge, cfg, FakeEPCAPI, caplog):
    caplog.set_level(logging.DEBUG)
    bridge.configure(str(cfg))
    name = bridge.complete_login(AUTH_CODE)

    assert name == DISPLAY_NAME
    assert FakeEPCAPI.received_auth_codes == [AUTH_CODE]
    saved = json.loads((cfg / "user.json").read_text())
    assert saved["account_id"] == ACCOUNT_ID and saved["displayName"] == DISPLAY_NAME
    assert AUTH_CODE not in caplog.text                       # never logged


@pytest.mark.parametrize("pasted", [f'"{AUTH_CODE}"', f"  {AUTH_CODE}\n",
                                    json.dumps({"redirectUrl": "x", "authorizationCode": AUTH_CODE,
                                                "exchangeCode": None, "sid": None})])
def test_complete_login_accepts_pasted_forms(bridge, cfg, FakeEPCAPI, pasted):
    bridge.configure(str(cfg))
    assert bridge.complete_login(pasted) == DISPLAY_NAME
    assert FakeEPCAPI.received_auth_codes == [AUTH_CODE]


@pytest.mark.parametrize("bad", ["", "   ", "{}", '{"authorizationCode": null}'])
def test_complete_login_rejects_empty(bridge, cfg, FakeEPCAPI, bad):
    bridge.configure(str(cfg))
    with pytest.raises(bridge.LoginError):
        bridge.complete_login(bad)
    assert FakeEPCAPI.received_auth_codes == []


def test_complete_login_rejected_by_epic(bridge, cfg, FakeEPCAPI):
    bridge.configure(str(cfg))
    with pytest.raises(bridge.LoginError) as e:
        bridge.complete_login("REJECT")
    assert e.value.code == "login_failed"
    assert not (cfg / "user.json").exists()


def test_unconfigured_use_is_an_error(bridge):
    with pytest.raises(bridge.NotConfiguredError):
        bridge.login_url()


# ------------------------------------------------------------------------------ launch args
LEGENDARY_ORDER = ["-AUTH_LOGIN=unused", f"-AUTH_PASSWORD={EXCHANGE_CODE}",
                   "-AUTH_TYPE=exchangecode", f"-epicapp={APP}", "-epicenv=Prod", "-EpicPortal",
                   f"-epicusername={DISPLAY_NAME}", f"-epicuserid={ACCOUNT_ID}"]


def test_launch_args_d005(bridge, cfg, game_dir, write_state, FakeEPCAPI):
    write_state()
    bridge.configure(str(cfg), game_dir=str(game_dir))
    r = bridge.launch_args()

    # D-005: RocketLeague.exe (not the installed Launcher.exe), switch removed
    assert r["exe"] == "Binaries/Win64/RocketLeague.exe"
    assert not any(a.lower() == OPTION_ARGS for a in r["argv"])
    assert not any("noeac" in a.lower() for a in r["argv"])
    assert r["cwd"] == str(game_dir / "Binaries" / "Win64")
    assert r["wine_exe"] == r"C:\Games\RocketLeague\Binaries\Win64\RocketLeague.exe"
    assert r["wine_cwd"] == r"C:\Games\RocketLeague\Binaries\Win64"
    assert r["game_dir"] == str(game_dir)
    assert not any(a.lower().endswith("wine") for a in r["argv"])  # no wine wrapper

    # the auth arguments Legendary generates (core.py:897-914), in Legendary's order
    for expected in LEGENDARY_ORDER:
        assert expected in r["argv"]
    idx = [r["argv"].index(a) for a in LEGENDARY_ORDER]
    assert idx == sorted(idx)
    assert any(a.startswith("-epiclocale=") for a in r["argv"])
    assert "-epicsandboxid=fakenamespace" in r["argv"]

    # session refreshed and fresh exchange code requested through Legendary
    assert "resume_session" in FakeEPCAPI.calls and "get_game_token" in FakeEPCAPI.calls


def test_launch_args_equal_legendarys_own_parameters_minus_switch(bridge, cfg, game_dir, write_state):
    write_state(launch_parameters="-nosplash -log")
    bridge.configure(str(cfg), game_dir=str(game_dir))
    r = bridge.launch_args()

    core = bridge._core()
    p = core.get_launch_parameters(APP, offline=False, extra_args=[OPTION_ARGS], disable_wine=True)
    legendary_full = p.game_parameters + p.user_parameters + p.egl_parameters
    assert legendary_full[:2] == ["-nosplash", "-log"]
    assert OPTION_ARGS in legendary_full                       # Legendary alone would pass it on
    assert r["argv"] == [a for a in legendary_full if a != OPTION_ARGS]


def test_option_args_come_from_metadata_not_code(bridge, cfg, game_dir, write_state):
    # Different case and an additional token: the switch is matched ignoring case and removed,
    # the extra token (which exists only in the metadata) is kept, in position.
    write_state(options=[("Other option", "-other"), (OPTION_NAME, "-NoEAC -fromMetadata")])
    bridge.configure(str(cfg), game_dir=str(game_dir))
    r = bridge.launch_args()
    assert r["exe"] == "Binaries/Win64/RocketLeague.exe"
    assert "-fromMetadata" in r["argv"]
    assert "-other" not in r["argv"]
    assert not any(a.lower() == "-noeac" for a in r["argv"])
    assert r["argv"].index("-fromMetadata") < r["argv"].index("-AUTH_LOGIN=unused")


@pytest.mark.parametrize("options", [
    [],                                                        # no options at all
    [("Some other option", "-x")],                             # option under another name
    [("launch without anti-cheat", "-noeac")],                 # name must match exactly
])
def test_missing_launch_option_is_a_clear_error_and_returns_nothing(
        bridge, cfg, game_dir, write_state, FakeEPCAPI, options):
    write_state(options=options)
    bridge.configure(str(cfg), game_dir=str(game_dir))
    with pytest.raises(bridge.LaunchOptionMissingError) as e:
        bridge.launch_args()
    assert e.value.code == "launch_option_missing"
    assert "Launch without Anti-Cheat" in str(e.value)
    assert "get_game_token" not in FakeEPCAPI.calls            # no exchange code was fetched


def test_missing_metadata_file_is_a_clear_error(bridge, cfg, game_dir, write_state, FakeEPCAPI):
    write_state(metadata=False)
    bridge.configure(str(cfg), game_dir=str(game_dir))
    with pytest.raises(bridge.LaunchOptionMissingError):
        bridge.launch_args()
    assert "get_game_token" not in FakeEPCAPI.calls


def test_option_without_known_switch_is_refused(bridge, cfg, game_dir, write_state, FakeEPCAPI):
    write_state(options=[(OPTION_NAME, "-someothermode")])
    bridge.configure(str(cfg), game_dir=str(game_dir))
    with pytest.raises(bridge.LaunchOptionUnmappedError):
        bridge.launch_args()


def test_offline_launch_skips_network(bridge, cfg, game_dir, write_state, FakeEPCAPI):
    write_state()
    bridge.configure(str(cfg), game_dir=str(game_dir))
    r = bridge.launch_args(offline=True)
    assert FakeEPCAPI.calls == []                              # no login, no token
    assert r["offline"] is True and r["can_run_offline"] is True
    assert "-AUTH_PASSWORD=" in r["argv"]                      # Legendary's empty password
    assert "-noeac" not in [a.lower() for a in r["argv"]]


def test_offline_per_game_config_key(bridge, cfg, game_dir, write_state, FakeEPCAPI):
    write_state()
    (cfg / "config.ini").write_text(f"[{APP}]\noffline = true\n")
    bridge.configure(str(cfg), game_dir=str(game_dir))
    assert bridge.launch_args()["offline"] is True             # cli.py:632 behaviour
    assert FakeEPCAPI.calls == []


def test_not_logged_in(bridge, cfg, game_dir, write_state):
    write_state(logged_in=False)
    bridge.configure(str(cfg), game_dir=str(game_dir))
    with pytest.raises(bridge.NotLoggedInError):
        bridge.launch_args()
    with pytest.raises(bridge.NotLoggedInError):
        bridge.launch_args(offline=True)


def test_not_registered(bridge, cfg, game_dir, write_state):
    write_state(installed=False)
    bridge.configure(str(cfg), game_dir=str(game_dir))
    with pytest.raises(bridge.GameNotInstalledError):
        bridge.launch_args()


def test_missing_game_exe(bridge, cfg, game_dir, write_state):
    write_state()
    (game_dir / "Binaries/Win64/RocketLeague.exe").unlink()
    bridge.configure(str(cfg), game_dir=str(game_dir))
    with pytest.raises(bridge.GameFilesError):
        bridge.launch_args()


def test_stale_install_path_is_repointed(bridge, cfg, game_dir, write_state, tmp_path):
    write_state(install_path=tmp_path / "old-container" / "game")
    bridge.configure(str(cfg), game_dir=str(game_dir))
    assert bridge.launch_args()["game_dir"] == str(game_dir)
    assert json.loads((cfg / "installed.json").read_text())[APP]["install_path"] == str(game_dir)


def test_redacted_argv(bridge):
    out = bridge.redacted_argv(["-AUTH_LOGIN=unused", "-AUTH_PASSWORD=SECRET", "-epicapp=Sugar"])
    assert "SECRET" not in " ".join(out) and "-epicapp=Sugar" in out


def test_status_is_offline(bridge, cfg, game_dir, write_state, FakeEPCAPI):
    write_state()
    bridge.configure(str(cfg), game_dir=str(game_dir))
    s = bridge.status()
    assert s == {"logged_in": True, "display_name": DISPLAY_NAME, "registered": True,
                 "install_path": str(game_dir), "metadata_cached": True,
                 "launch_option_available": True}
    assert FakeEPCAPI.calls == []


# ------------------------------------------------------------------------------ register
def test_register_game_import_flow(bridge, cfg, game_dir, write_state, FakeEPCAPI):
    write_state(installed=False, metadata=False)
    MANIFEST["bytes"] = json_manifest(["Binaries/Win64/Launcher.exe", "Binaries/Win64/RocketLeague.exe"])
    bridge.configure(str(cfg), game_dir=str(game_dir))

    bridge.register_game(str(game_dir))

    installed = json.loads((cfg / "installed.json").read_text())[APP]
    assert installed["install_path"] == str(game_dir)
    assert installed["executable"] == "Binaries/Win64/Launcher.exe"   # D-002
    meta = json.loads((cfg / "metadata" / f"{APP}.json").read_text())
    assert meta["metadata"]["customAttributes"]["extraLaunchOption_001_Name"]["value"] == OPTION_NAME
    assert {"resume_session", "get_game_assets", "get_game_manifest"} <= set(FakeEPCAPI.calls)

    # and the registered game launches with the metadata that registration cached
    r = bridge.launch_args()
    assert r["exe"] == "Binaries/Win64/RocketLeague.exe"
    assert f"-AUTH_PASSWORD={EXCHANGE_CODE}" in r["argv"]


def test_register_game_is_idempotent_and_repoints(bridge, cfg, game_dir, write_state, FakeEPCAPI, tmp_path):
    write_state(install_path=tmp_path / "gone")
    bridge.configure(str(cfg))
    bridge.register_game(str(game_dir))
    assert FakeEPCAPI.calls == []                                     # no network needed
    assert json.loads((cfg / "installed.json").read_text())[APP]["install_path"] == str(game_dir)


def test_register_game_requires_login(bridge, cfg, game_dir, write_state):
    write_state(installed=False, metadata=False, logged_in=False)
    bridge.configure(str(cfg))
    with pytest.raises(bridge.NotLoggedInError):
        bridge.register_game(str(game_dir))


def test_register_game_wrong_folder(bridge, cfg, tmp_path, write_state, FakeEPCAPI):
    write_state(installed=False, metadata=False)
    MANIFEST["bytes"] = json_manifest(["Binaries/Win64/Launcher.exe"])
    empty = tmp_path / "empty"
    empty.mkdir()
    bridge.configure(str(cfg))
    with pytest.raises(bridge.GameFilesError):
        bridge.register_game(str(empty))
    with pytest.raises(bridge.GameFilesError):
        bridge.register_game(str(tmp_path / "does-not-exist"))
    assert not (cfg / "installed.json").exists()


# ------------------------------------------------------------------------------ crypto fallback
def test_module_imports_without_pycryptodomex(tmp_path):
    """Legendary imports Cryptodome at module level; the bridge must still load without it."""
    cfgdir = tmp_path / "c"
    script = textwrap.dedent(f"""
        import importlib.abc, importlib.util, sys
        class Block(importlib.abc.MetaPathFinder):
            def find_spec(self, name, path, target=None):
                if name == "Cryptodome" or name.startswith("Cryptodome."):
                    raise ImportError("blocked for test")
        sys.meta_path.insert(0, Block())
        spec = importlib.util.spec_from_file_location("b", {str(os.path.join(os.path.dirname(__file__), '..', '..', 'ios', 'legendary', 'bridge.py'))!r})
        b = importlib.util.module_from_spec(spec); spec.loader.exec_module(b)
        b.configure({str(cfgdir)!r})
        print(b.login_url())
        from Cryptodome.Cipher import AES
        try:
            AES.new(b"k" * 16, AES.MODE_GCM, nonce=b"n" * 12)
        except RuntimeError as e:
            print("RuntimeError:", e)
    """)
    env = {k: v for k, v in os.environ.items() if k not in ("PYTHONPATH",)}
    out = subprocess.run([sys.executable, "-I", "-c", script], env=env, capture_output=True,
                         text=True, timeout=120)
    assert out.returncode == 0, out.stderr
    lines = out.stdout.splitlines()
    assert lines[0] == LEGENDARY_GL_REDIRECT
    assert lines[1].startswith("RuntimeError:") and "cannot decrypt" in lines[1]
