#!/usr/bin/env python3
"""panel-hub — subscribes to ntfy topics and fans notifications out to a
fleet of CrowPanel ESP32 devices over the NDJSON/TCP device-link protocol,
then executes device-triggered ntfy http actions back at ntfy/producers.

This is the deployable second consumer of the same device-link protocol the
local claude bridge speaks (see bridge/bridge.py). It reuses that skeleton:
the asyncio TCP device server, NDJSON send/readline, and the log helper.
Permission round-trips and tmux injection stay local to the bridge; the hub
drops those message types with a note.

Wire protocol (hub -> device):
  notify   {id, topic, view, priority, ttl, title, body, icon, color,
            actions:[{label}], value, max, unit, label, items[], points[]}
Wire protocol (device -> hub):
  notify_action {id, action:<idx>}  -> fire stored ntfy http action
  notify_action {id, dismiss:true}  -> log only
  hello {name, fw, ...}             -> register/replace device by name

Config: JSON at $PANEL_CONFIG (default /etc/panel-hub/config.json, then
./config.json). Env overrides: NTFY_URL, DEVICE_PORT, HTTP_PORT.
STATE_FILE (optional) persists the last-seen ntfy message id so a restart
resumes with since= instead of dropping everything published while down.
Health: GET /health on HTTP_PORT.
"""
import asyncio
import json
import os
import sys
import time
from urllib.parse import urlparse

import aiohttp
from aiohttp import web

DEFAULT_CONFIG = {
    "ntfy_url": "https://ntfy.aph.run",
    "devices": {
        "office": {"topics": ["infra-alerts", "infra-status"]},
        "den":    {"topics": ["infra-alerts"]},
        "rack":   {"topics": ["infra-status", "pipeline"]},
        "desk":   {"topics": ["pipeline", "claude"]},
    },
    "device_port": 8788,
    "http_port": 8080,
    # http actions only fire at these host suffixes (SSRF guard: a producer
    # with publish access must not be able to aim the hub's network identity
    # at cluster-internal endpoints)
    "allowed_action_hosts": ["aph.run"],
}

DEFAULT_TTL_S = 1800          # 30 min, matches the firmware card default
TEMPLATE_VIEWS = ("card", "gauge", "list", "graph")

# ntfy sends a keepalive roughly every 45s; a longer read gap means a dead
# stream, so reconnect. total must be disabled or the 300s default kills it.
SUB_TIMEOUT = aiohttp.ClientTimeout(total=None, sock_connect=30, sock_read=120)
ACTION_TIMEOUT = aiohttp.ClientTimeout(total=30)


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", file=sys.stdout, flush=True)


def load_config():
    primary = os.environ.get("PANEL_CONFIG", "/etc/panel-hub/config.json")
    here = os.path.dirname(os.path.abspath(__file__))
    cfg = json.loads(json.dumps(DEFAULT_CONFIG))  # deep copy of defaults
    loaded = None
    for path in (primary, os.path.join(here, "config.json"), "./config.json"):
        try:
            with open(path) as f:
                cfg.update(json.load(f))
            loaded = path
            break
        except FileNotFoundError:
            continue
        except Exception as e:
            log(f"config load failed for {path} ({e}), trying next")
    log(f"config loaded from {loaded}" if loaded else "no config file found, using defaults")

    if os.environ.get("NTFY_URL"):
        cfg["ntfy_url"] = os.environ["NTFY_URL"]
    if os.environ.get("DEVICE_PORT"):
        cfg["device_port"] = int(os.environ["DEVICE_PORT"])
    if os.environ.get("HTTP_PORT"):
        cfg["http_port"] = int(os.environ["HTTP_PORT"])
    # live ntfy requires auth; token comes from env (k8s secret / op-injected)
    # or NTFY_TOKEN_FILE (op-style /run/op mount), never from git
    if os.environ.get("NTFY_TOKEN"):
        cfg["ntfy_token"] = os.environ["NTFY_TOKEN"]
    elif os.environ.get("NTFY_TOKEN_FILE"):
        try:
            with open(os.environ["NTFY_TOKEN_FILE"]) as f:
                cfg["ntfy_token"] = f.read().strip()
        except OSError as e:
            log(f"NTFY_TOKEN_FILE unreadable: {e}")
    return cfg


class Hub:
    def __init__(self, cfg):
        self.cfg = cfg
        self.routes = {
            name: list((d or {}).get("topics", []))
            for name, d in cfg.get("devices", {}).items()
        }
        topics = set()
        for tl in self.routes.values():
            topics.update(tl)
        self.topics = sorted(topics)

        self.devices = {}       # name -> StreamWriter (only known, routable names)
        self.actions = {}       # msg_id -> {"specs": [ntfy action dicts], "exp": ts}
        self.session = None     # aiohttp.ClientSession, created in the loop
        self.ntfy_connected = False
        self.state_file = os.environ.get("STATE_FILE")
        self.last_id = self._load_state()  # last ntfy message id, for since= resume

    def _load_state(self):
        if not self.state_file:
            return None
        try:
            with open(self.state_file) as f:
                return f.read().strip() or None
        except OSError:
            return None

    def _save_state(self):
        if not self.state_file or not self.last_id:
            return
        try:
            tmp = self.state_file + ".tmp"
            with open(tmp, "w") as f:
                f.write(self.last_id)
            os.replace(tmp, self.state_file)
        except OSError as e:
            log(f"state file write failed: {e}")

    # ---- device link (TCP server, device dials in) ----

    async def device_server(self):
        server = await asyncio.start_server(
            self.on_device_conn, "0.0.0.0", self.cfg["device_port"]
        )
        log(f"device server on 0.0.0.0:{self.cfg['device_port']}")
        async with server:
            await server.serve_forever()

    async def on_device_conn(self, reader, writer):
        peer = writer.get_extra_info("peername") or ("?", 0)
        log(f"device connected from {peer[0]}")
        name = None
        try:
            while True:
                line = await reader.readline()
                if not line:
                    break
                try:
                    m = json.loads(line)
                except ValueError:
                    continue
                if m.get("t") == "hello":
                    name = self.register_device(m, writer, peer)
                else:
                    await self.on_device_msg(name, m)
        except Exception as e:
            log(f"device conn error: {type(e).__name__}: {e}")
        finally:
            if name and self.devices.get(name) is writer:
                self.devices.pop(name, None)
                log(f"device '{name}' disconnected")
            try:
                writer.close()
            except Exception:
                pass

    def register_device(self, m, writer, peer):
        name = m.get("name")
        if not name or name not in self.routes:
            log(f"device hello with unknown/missing name {name!r} from {peer[0]} "
                f"— connection kept, routing nothing")
            return None  # not added to self.devices; router will never touch it
        old = self.devices.get(name)
        if old is not None and old is not writer:
            log(f"device '{name}' reconnected — replacing previous connection")
            try:
                old.close()
            except Exception:
                pass
        self.devices[name] = writer
        log(f"device '{name}' registered: fw {m.get('fw')} from {peer[0]} "
            f"(topics {self.routes.get(name)})")
        return name

    async def on_device_msg(self, name, m):
        t = m.get("t")
        if t == "notify_action":
            await self.handle_notify_action(name, m)
        elif t == "dbg":
            log(f"device '{name}' dbg: {m.get('m')}")
        elif t in ("perm_result", "inject"):
            log(f"device '{name}' {t} dropped — claude perm/inject flows are "
                f"local-bridge only for now")
        else:
            log(f"device '{name}' unhandled msg t={t!r}")

    async def send(self, writer, obj):
        try:
            writer.write((json.dumps(obj) + "\n").encode())
            await writer.drain()
        except Exception as e:
            log(f"send failed: {e}")

    # ---- device -> hub actions ----

    def action_url_allowed(self, url):
        try:
            p = urlparse(url)
        except ValueError:
            return False
        if p.scheme not in ("http", "https") or not p.hostname:
            return False
        host = p.hostname.lower()
        return any(
            host == suf or host.endswith("." + suf)
            for suf in (self.cfg.get("allowed_action_hosts") or [])
        )

    async def handle_notify_action(self, name, m):
        mid = m.get("id")
        if m.get("dismiss"):
            log(f"device '{name}' dismissed notify id={mid}")
            self.actions.pop(mid, None)
            return
        idx = m.get("action")
        entry = self.actions.get(mid)
        specs = entry["specs"] if entry else []
        if not isinstance(idx, int) or not 0 <= idx < len(specs):
            log(f"device '{name}' notify_action id={mid} action={idx} "
                f"— no stored action, ignoring")
            return
        spec = specs[idx]
        atype = spec.get("action") or spec.get("type")
        if atype != "http":
            log(f"device '{name}' notify_action id={mid} action={idx} "
                f"— stored action is '{atype}', not http; ignoring")
            return
        method = (spec.get("method") or "POST").upper()
        url = spec.get("url")
        headers = spec.get("headers") or {}
        body = spec.get("body")
        if not url:
            log(f"notify_action id={mid} action={idx} — http action has no url")
            return
        if not self.action_url_allowed(url):
            log(f"notify_action id={mid} action={idx} — url {url!r} rejected "
                f"(scheme/host outside allowed_action_hosts)")
            return
        try:
            async with self.session.request(
                method, url, headers=headers, data=body, timeout=ACTION_TIMEOUT
            ) as resp:
                await resp.read()
                log(f"action id={mid} idx={idx}: {method} {url} -> {resp.status}")
        except Exception as e:
            log(f"action id={mid} idx={idx}: {method} {url} failed: "
                f"{type(e).__name__}: {e}")

    # ---- ntfy message -> device notify wire message ----

    def build_notify(self, msg):
        mid = msg.get("id")
        topic = msg.get("topic")
        body_text = msg.get("message") or ""

        parsed = None
        if body_text:
            try:
                cand = json.loads(body_text)
            except (ValueError, TypeError):
                cand = None
            if isinstance(cand, dict) and cand.get("view") in TEMPLATE_VIEWS:
                parsed = cand

        if parsed is not None:
            wire = dict(parsed)            # pass template fields through
            wire["t"] = "notify"
        else:
            wire = {
                "t": "notify",
                "view": "card",
                "title": msg.get("title") or "",
                "body": body_text,
            }

        wire["id"] = mid
        wire["topic"] = topic
        wire["priority"] = msg.get("priority") or 3
        wire["ttl"] = self._ttl(msg)

        # tags: panel-color:RRGGBB -> accent color (overrides); first remaining -> icon
        icon = None
        for tag in (msg.get("tags") or []):
            if not isinstance(tag, str):
                continue
            if tag.startswith("panel-color:"):
                hexpart = tag.split(":", 1)[1].strip()
                if hexpart:
                    wire["color"] = "#" + hexpart
            elif icon is None:
                icon = tag
        if icon is not None and not wire.get("icon"):
            wire["icon"] = icon

        # actions: send labels only; keep full specs hub-side keyed by msg id.
        # Compact non-dict entries out BEFORE storing so device indexes and
        # stored indexes always agree.
        actions = msg.get("actions")
        if isinstance(actions, list):
            specs = [a for a in actions if isinstance(a, dict)]
            if specs:
                wire["actions"] = [{"label": a.get("label", "")} for a in specs]
                self.actions[mid] = {
                    "specs": specs,
                    "exp": time.time() + wire["ttl"] + 60,
                }

        return wire

    @staticmethod
    def _ttl(msg):
        expires = msg.get("expires")
        if isinstance(expires, (int, float)) and expires > 0:
            base = msg.get("time")
            if not isinstance(base, (int, float)):
                base = int(time.time())
            return int(max(1, expires - base))
        return DEFAULT_TTL_S

    async def route(self, msg):
        topic = msg.get("topic")
        if not topic:
            return
        wire = self.build_notify(msg)
        sent = 0
        for name, topics in self.routes.items():
            if topic in topics and name in self.devices:
                await self.send(self.devices[name], wire)
                sent += 1
        log(f"routed ntfy id={msg.get('id')} topic={topic} "
            f"view={wire.get('view')} -> {sent} device(s)")

    # ---- ntfy streaming subscriber ----

    async def ntfy_subscriber(self):
        if not self.topics:
            log("no topics configured; ntfy subscriber idle")
            return
        topic_path = ",".join(self.topics)
        base = self.cfg["ntfy_url"].rstrip("/")
        backoff = 1
        while True:
            params = {}
            if self.last_id:
                params["since"] = self.last_id
            url = f"{base}/{topic_path}/json"
            try:
                log(f"ntfy connecting: {url}"
                    + (f" (since={self.last_id})" if self.last_id else ""))
                headers = {}
                if self.cfg.get("ntfy_token"):
                    headers["Authorization"] = f"Bearer {self.cfg['ntfy_token']}"
                async with self.session.get(
                    url, params=params, headers=headers, timeout=SUB_TIMEOUT
                ) as resp:
                    if resp.status != 200:
                        log(f"ntfy subscribe returned HTTP {resp.status}")
                        raise aiohttp.ClientError(f"HTTP {resp.status}")
                    self.ntfy_connected = True
                    backoff = 1
                    log(f"ntfy connected, streaming topics: {topic_path}")
                    async for raw in resp.content:
                        line = raw.strip()
                        if not line:
                            continue
                        try:
                            ev = json.loads(line)
                        except ValueError:
                            continue
                        if ev.get("event") != "message":
                            continue  # open / keepalive / poll_request
                        if ev.get("id"):
                            self.last_id = ev["id"]
                            self._save_state()
                        await self.route(ev)
            except asyncio.CancelledError:
                raise
            except Exception as e:
                log(f"ntfy subscriber error: {type(e).__name__}: {e}")
            finally:
                self.ntfy_connected = False
            log(f"ntfy reconnecting in {backoff}s")
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, 30)

    # ---- housekeeping ----

    async def prune_actions(self):
        while True:
            await asyncio.sleep(60)
            now = time.time()
            for mid, entry in list(self.actions.items()):
                if now > entry["exp"]:
                    self.actions.pop(mid, None)

    # ---- HTTP (health) ----

    async def h_health(self, req):
        return web.json_response({
            "devices": {name: (name in self.devices) for name in self.routes},
            "topics": self.topics,
            "ntfy_connected": self.ntfy_connected,
        })


def main():
    cfg = load_config()
    hub = Hub(cfg)
    log(f"ntfy_url={cfg['ntfy_url']} topics={hub.topics} "
        f"devices={list(hub.routes)}")

    app = web.Application()
    app.router.add_get("/health", hub.h_health)

    async def start():
        hub.session = aiohttp.ClientSession(timeout=ACTION_TIMEOUT)
        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, "0.0.0.0", cfg["http_port"])
        await site.start()
        log(f"http (health) on 0.0.0.0:{cfg['http_port']}")
        try:
            await asyncio.gather(
                hub.device_server(), hub.ntfy_subscriber(), hub.prune_actions()
            )
        finally:
            await hub.session.close()

    try:
        asyncio.run(start())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
