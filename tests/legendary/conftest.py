"""Fixtures for the Legendary bridge tests.

Everything is local: Legendary's API class is replaced by ``FakeEPCAPI`` (a subclass of the
real ``EPCAPI`` that only overrides the methods which would hit Epic), ``LGDAPI`` is
replaced as well, and any real HTTP request to a non-loopback host fails the test.  The
config directory, the "game" and Epic's metadata are fabricated in ``tmp_path``.  No real
tokens, no real game files.
"""
from __future__ import annotations

import hashlib
import importlib.util
import json
import os
import sys
from pathlib import Path
from urllib.parse import urlparse

import pytest
import requests

REPO = Path(__file__).resolve().parents[2]
BRIDGE_PATH = REPO / "ios" / "legendary" / "bridge.py"

APP = "Sugar"
EXCHANGE_CODE = "FAKE-EXCHANGE-CODE-0001"
AUTH_CODE = "FAKEAUTHORIZATIONCODE0123456789ab"
ACCOUNT_ID = "fakeaccountid0000000000000000000"
DISPLAY_NAME = "TestPlayer"
FAR_FUTURE = "2099-01-01T00:00:00.000Z"

# Shape of Epic's catalog attributes as cached by Legendary (core.py: fetch_game_meta ->
# lgd.set_game_meta).  Names/args are fabricated here, bridge code must read them.
OPTION_NAME = "Launch without Anti-Cheat"
OPTION_ARGS = "-noeac"


def custom_attributes(options):
    attrs = {"CanRunOffline": {"type": "STRING", "value": "true"}}
    for i, (name, args) in enumerate(options, start=1):
        attrs[f"extraLaunchOption_{i:03d}_Name"] = {"type": "STRING", "value": name}
        attrs[f"extraLaunchOption_{i:03d}_Args"] = {"type": "STRING", "value": args}
    return attrs


def game_metadata(options):
    return {
        "id": "catalogitemid000000000000000000",
        "title": "Rocket League (fake)",
        "namespace": "fakenamespace",
        "customAttributes": custom_attributes(options),
        "categories": [{"path": "games"}, {"path": "applications"}],
    }


def json_manifest(files):
    return json.dumps({
        "ManifestFileVersion": "013000000000", "bIsFileData": False, "AppID": "000000000000",
        "AppNameString": APP, "BuildVersionString": "1.0.fake",
        "LaunchExeString": "Binaries/Win64/Launcher.exe", "LaunchCommand": "",
        "PrereqIds": [], "PrereqName": "", "PrereqPath": "", "PrereqArgs": "",
        "FileManifestList": [{"Filename": f, "FileHash": "000" * 20, "FileChunkParts": []}
                             for f in files],
        "ChunkFilesizeList": {}, "ChunkHashList": {}, "ChunkShaList": {}, "DataGroupList": {},
        "CustomFields": {},
    }).encode()


# Bytes the fake CDN serves for the manifest URL; tests set MANIFEST["bytes"].
MANIFEST = {"bytes": b""}


class FakeSession(requests.Session):
    """Real Session (so TLS settings are real); serves the fake CDN manifest URL."""

    def get(self, url, **kw):
        if url.startswith("https://cdn.example.test/"):
            r = requests.Response()
            r.status_code = 200
            r._content = MANIFEST["bytes"]
            return r
        return super().get(url, **kw)


def _fake_api_class():
    from legendary.api.egs import EPCAPI

    class FakeEPCAPI(EPCAPI):
        """Only the methods that would contact Epic are overridden."""

        calls: list = []
        received_auth_codes: list = []

        def __init__(self, *a, **kw):
            super().__init__(*a, **kw)
            self.unauth_session = FakeSession()

        def resume_session(self, session):
            self.calls.append("resume_session")
            self.user = session
            return session

        def start_session(self, refresh_token=None, exchange_token=None,
                          authorization_code=None, client_credentials=False):
            self.calls.append("start_session")
            if authorization_code is not None:
                self.received_auth_codes.append(authorization_code)
                if authorization_code == "REJECT":
                    from legendary.models.exceptions import InvalidCredentialsError
                    raise InvalidCredentialsError("errors.com.epicgames.fake.rejected")
            j = {"access_token": "fake-access", "refresh_token": "fake-refresh",
                 "expires_at": FAR_FUTURE, "refresh_expires_at": FAR_FUTURE,
                 "account_id": ACCOUNT_ID, "displayName": DISPLAY_NAME}
            self.user = j
            return j

        def get_game_token(self):
            self.calls.append("get_game_token")
            return {"code": EXCHANGE_CODE}

        def get_ownership_token(self, namespace, catalog_item_id):
            self.calls.append("get_ownership_token")
            return b"fake-ovt"

        def get_game_assets(self, platform="Windows", label="Live"):
            self.calls.append("get_game_assets")
            return [{"appName": APP, "assetId": "fakeasset", "buildVersion": "1.0.fake",
                     "catalogItemId": "catalogitemid000000000000000000",
                     "labelName": "Live-Windows", "namespace": "fakenamespace",
                     "metadata": {}}]

        def get_user_entitlements_full(self):
            return []

        def get_game_info(self, namespace, catalog_item_id, timeout=None):
            self.calls.append("get_game_info")
            return game_metadata([(OPTION_NAME, OPTION_ARGS)])

        def get_game_achievements(self, namespace):
            return {"data": {"Achievement": {"productAchievementsRecordBySandbox": {
                "sandboxId": namespace, "totalAchievements": 0, "totalProductXP": 0,
                "achievementSets": [], "platinumRarity": {}, "achievements": []}}}}

        def get_game_manifest(self, namespace, catalog_item_id, app_name,
                              platform="Windows", label="Live"):
            self.calls.append("get_game_manifest")
            return {"elements": [{"hash": hashlib.sha1(MANIFEST["bytes"]).hexdigest(),
                                  "manifests": [{"uri": "https://cdn.example.test/Sugar/m.manifest"}]}]}

    return FakeEPCAPI


class FakeLGDAPI:
    """Replaces Legendary's own update-check API (api.legendary.gl)."""

    def __init__(self):
        self.session = requests.Session()

    def get_version_information(self):
        return {"release_info": {"version": "0.0.1"}}


@pytest.fixture(autouse=True)
def no_real_network(monkeypatch):
    real_send = requests.adapters.HTTPAdapter.send

    def guard(self, request, *a, **kw):
        host = urlparse(request.url).hostname
        if host not in ("localhost", "127.0.0.1", "::1"):
            raise AssertionError(f"test attempted a real network request to {request.url!r}")
        return real_send(self, request, *a, **kw)

    monkeypatch.setattr(requests.adapters.HTTPAdapter, "send", guard)
    for var in ("HTTP_PROXY", "HTTPS_PROXY", "http_proxy", "https_proxy", "ALL_PROXY", "all_proxy"):
        monkeypatch.delenv(var, raising=False)


@pytest.fixture
def FakeEPCAPI(monkeypatch):
    import legendary.core
    cls = _fake_api_class()
    cls.calls, cls.received_auth_codes = [], []
    monkeypatch.setattr(legendary.core, "EPCAPI", cls)
    monkeypatch.setattr(legendary.core, "LGDAPI", FakeLGDAPI)
    return cls


@pytest.fixture
def cfg(tmp_path):
    d = tmp_path / "legendary-config"
    d.mkdir()
    return d


@pytest.fixture
def game_dir(tmp_path):
    d = tmp_path / "game"
    for rel in ("Binaries/Win64/Launcher.exe", "Binaries/Win64/RocketLeague.exe",
                "Binaries/Win64/RocketLeague_EAC.exe"):
        p = d / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(b"")  # empty placeholders, never real game bytes
    return d


@pytest.fixture
def bridge(monkeypatch, FakeEPCAPI, cfg, tmp_path):
    """A fresh bridge module instance whose env-var changes are undone after the test."""
    for var in ("LEGENDARY_CONFIG_PATH", "REQUESTS_CA_BUNDLE", "SSL_CERT_FILE"):
        monkeypatch.setenv(var, "unset-by-test")  # so teardown restores the original state
    spec = importlib.util.spec_from_file_location("rl_bridge_under_test", BRIDGE_PATH)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    yield mod
    sys.modules.pop(spec.name, None)


@pytest.fixture
def write_state(cfg, game_dir):
    """Writes fake installed.json / metadata/Sugar.json / user.json into the config dir."""
    def _write(options=((OPTION_NAME, OPTION_ARGS),), logged_in=True, installed=True,
               metadata=True, launch_parameters="", install_path=None):
        if installed:
            (cfg / "installed.json").write_text(json.dumps({APP: {
                "app_name": APP, "install_path": str(install_path or game_dir),
                "title": "Rocket League (fake)", "version": "1.0.fake",
                "executable": "Binaries/Win64/Launcher.exe", "can_run_offline": True,
                "launch_parameters": launch_parameters, "requires_ot": False,
                "platform": "Windows", "is_dlc": False}}))
        if metadata:
            (cfg / "metadata").mkdir(exist_ok=True)
            (cfg / "metadata" / f"{APP}.json").write_text(json.dumps({
                "app_name": APP, "app_title": "Rocket League (fake)", "asset_infos": {},
                "base_urls": [], "metadata": game_metadata(list(options)),
                "sidecar": None, "achievements": None}))
        if logged_in:
            (cfg / "user.json").write_text(json.dumps({
                "access_token": "fake-access", "refresh_token": "fake-refresh",
                "expires_at": FAR_FUTURE, "account_id": ACCOUNT_ID,
                "displayName": DISPLAY_NAME}))
    return _write
