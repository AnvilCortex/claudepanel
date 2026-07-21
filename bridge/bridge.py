#!/usr/bin/env python3
"""claudepanel bridge — relays Claude Code session events to the CrowPanel
ESP32 device over WiFi and routes device actions (permission decisions,
text injection via tmux) back into running sessions.

HTTP API (127.0.0.1 only) used by Claude Code statusline/hook scripts:
  POST /statusline        raw statusline JSON from Claude Code
  POST /hook/{event}      {"hook": <hook stdin json>, "tmux_pane": "%N"}
  POST /permission        blocks until the device answers (or timeout);
                          responds {"decision": "allow"|"deny"|"pass"}
  GET  /health
"""
import asyncio
import json
import os
import shutil
import subprocess
import sys
import time
import uuid

import aiohttp
from aiohttp import web
from urllib.parse import urlparse

CONFIG_PATH = os.path.expanduser("~/.config/claudepanel/config.json")
DEFAULT_CONFIG = {
    "listen_port": 8788,  # device dials in here over plain TCP (NDJSON)
    "http_port": 8787,
    "enc_div": 4,
    "beep": False,
    "leds": False,
    "perm_timeout_s": 55,
    # optional: publish a session-summary template to ntfy so hub-connected
    # panels can show claude state; empty topic disables
    "ntfy_url": "https://ntfy.aph.run",
    "ntfy_claude_topic": "",
    "ntfy_token": "",  # live ntfy requires auth; keep out of git
    "presets": [
        {"label": "Continue", "text": "continue"},
        {"label": "Run tests", "text": "run the tests and fix any failures"},
        {"label": "Explain", "text": "explain what you just did in two sentences"},
        {"label": "Commit", "text": "commit the current work"},
        {"label": "Compact", "text": "/compact"},
    ],
}

SESSION_TTL_S = 45 * 60

# launchd services get a minimal PATH; find tmux in the usual places
TMUX_BIN = shutil.which("tmux") or next(
    (p for p in ("/opt/homebrew/bin/tmux", "/usr/local/bin/tmux", "/usr/bin/tmux")
     if os.path.exists(p)),
    None,
)


def log(msg):
    print(f"[{time.strftime('%H:%M:%S')}] {msg}", file=sys.stderr, flush=True)


def load_config():
    cfg = dict(DEFAULT_CONFIG)
    try:
        with open(CONFIG_PATH) as f:
            cfg.update(json.load(f))
    except FileNotFoundError:
        os.makedirs(os.path.dirname(CONFIG_PATH), exist_ok=True)
        with open(CONFIG_PATH, "w") as f:
            json.dump(DEFAULT_CONFIG, f, indent=2)
        log(f"wrote default config to {CONFIG_PATH}")
    except Exception as e:
        log(f"config load failed ({e}), using defaults")
    return cfg


def tool_summary(tool, tool_input):
    """One-line human summary of a tool call."""
    ti = tool_input if isinstance(tool_input, dict) else {}
    if tool == "Bash":
        return ti.get("command", "")[:90]
    if tool in ("Edit", "Write", "Read", "NotebookEdit"):
        return os.path.basename(ti.get("file_path", ""))[:60]
    if tool in ("Glob", "Grep"):
        return ti.get("pattern", "")[:60]
    if tool in ("WebFetch", "WebSearch"):
        return (ti.get("url") or ti.get("query") or "")[:70]
    if tool in ("Task", "Agent"):
        return ti.get("description", "")[:60]
    for key in ("command", "path", "query", "prompt"):
        if key in ti:
            return str(ti[key])[:70]
    return ""


class Bridge:
    def __init__(self, cfg):
        self.cfg = cfg
        self.sessions = {}  # sid -> dict
        self.writer = None  # active device connection
        self.pending = {}  # perm id -> Future
        self.perm_lock = asyncio.Lock()
        self.notif_seq = 0
        self.notif_actions = {}  # notif id -> [full action specs]
        self.last_ntfy_pub = 0.0
        self._http = None

    def http(self):
        if self._http is None or self._http.closed:
            self._http = aiohttp.ClientSession(
                timeout=aiohttp.ClientTimeout(total=10))
        return self._http

    # ---- session registry ----

    def sess(self, sid):
        return self.sessions.setdefault(
            sid,
            {"title": "?", "model": "", "state": "idle", "tool": "",
             "cost": 0.0, "ctx": 0, "pane": None, "last": time.time()},
        )

    # ---- device link (TCP server, device dials in) ----

    async def device_server(self):
        server = await asyncio.start_server(
            self.on_device_conn, "0.0.0.0", self.cfg["listen_port"]
        )
        log(f"device server on 0.0.0.0:{self.cfg['listen_port']}")
        async with server:
            await server.serve_forever()

    async def on_device_conn(self, reader, writer):
        peer = writer.get_extra_info("peername") or ("?", 0)
        log(f"device connected from {peer[0]}")
        if self.writer is not None:  # newest connection wins
            try:
                self.writer.close()
            except Exception:
                pass
        self.writer = writer
        try:
            while True:
                line = await reader.readline()
                if not line:
                    break
                try:
                    m = json.loads(line)
                except ValueError:
                    continue
                await self.on_device_msg(m)
        except Exception as e:
            log(f"device conn error: {type(e).__name__}: {e}")
        finally:
            if self.writer is writer:
                self.writer = None
                for pid, fut in list(self.pending.items()):
                    if not fut.done():
                        fut.set_result("pass")
                    self.pending.pop(pid, None)
                log("device disconnected")
            try:
                writer.close()
            except Exception:
                pass

    async def send(self, obj):
        w = self.writer
        if w is None:
            return
        try:
            w.write((json.dumps(obj) + "\n").encode())
            await w.drain()
        except Exception as e:
            log(f"send failed: {e}")

    async def push_config(self):
        await self.send(
            {"t": "cfg", "enc_div": self.cfg["enc_div"], "beep": self.cfg["beep"],
             "leds": self.cfg.get("leds", False)}
        )
        items = [
            {"label": p["label"][:20], "text": p["text"][:138]}
            for p in self.cfg["presets"][:10]
        ]
        await self.send({"t": "presets", "items": items})

    async def push_session(self, sid):
        s = self.sessions.get(sid)
        if not s:
            return
        await self.send(
            {"t": "status", "sid": sid, "title": s["title"], "model": s["model"],
             "state": s["state"], "tool": s["tool"], "cost": s["cost"], "ctx": s["ctx"]}
        )

    async def push_all_sessions(self):
        for sid in list(self.sessions):
            await self.push_session(sid)

    async def on_device_msg(self, m):
        t = m.get("t")
        if t == "perm_result":
            fut = self.pending.pop(m.get("id", ""), None)
            if fut and not fut.done():
                fut.set_result(m.get("result", "pass"))
        elif t == "inject":
            await self.inject(m.get("sid"), m.get("text", ""))
        elif t == "notify_action":
            nid = m.get("id", "")
            if m.get("dismiss"):
                log(f"notify {nid} dismissed on device")
                self.notif_actions.pop(nid, None)
            else:
                idx = m.get("action", 0)
                acts, _exp = self.notif_actions.get(nid) or ([], 0)
                if isinstance(idx, int) and 0 <= idx < len(acts) and acts[idx].get("url"):
                    asyncio.create_task(self.fire_action(nid, acts[idx]))
                else:
                    log(f"notify action {nid}[{idx}]: nothing to execute")
        elif t == "dbg":
            log(f"device dbg: {m.get('m')}")
        elif t == "hello":
            # rst codes (esp_reset_reason): 1 power-on, 3 sw restart,
            # 4 panic, 5 int wdt, 6 task wdt, 7 other wdt, 15 brownout
            log(f"device hello: fw {m.get('fw')} rst={m.get('rst')} up={m.get('up')}s")
            # device (re)booted — resync full state
            await self.push_config()
            await self.push_all_sessions()

    # ---- tmux injection ----

    async def inject(self, sid, text):
        if not text:
            return
        s = self.sessions.get(sid or "")
        pane = s.get("pane") if s else None
        if not pane:
            log("inject dropped: session has no tmux pane (run claude inside tmux)")
            await self.send({"t": "toast", "m": "not in tmux"})
            return
        if not TMUX_BIN:
            log("inject dropped: tmux not installed")
            await self.send({"t": "toast", "m": "no tmux on host"})
            return
        r1 = subprocess.run([TMUX_BIN, "send-keys", "-t", pane, "-l", text],
                            capture_output=True, text=True)
        if r1.returncode != 0:
            log(f"inject failed: {r1.stderr.strip()}")
            await self.send({"t": "toast", "m": "inject failed"})
            return
        subprocess.run([TMUX_BIN, "send-keys", "-t", pane, "Enter"], capture_output=True)
        log(f"injected into {pane}: {text[:50]!r}")
        await self.send({"t": "toast", "m": "injected"})

    async def fire_action(self, nid, a):
        label = a.get("label", "?")
        if urlparse(a["url"]).scheme not in ("http", "https"):
            log(f"action {label!r} for {nid}: non-http url rejected")
            await self.send({"t": "toast", "m": f"{label}: bad url"[:34]})
            return
        try:
            async with self.http().request(
                a.get("method", "POST").upper(), a["url"],
                headers=a.get("headers") or {}, data=a.get("body"),
            ) as r:
                log(f"action {label!r} for {nid}: HTTP {r.status}")
                await self.send({"t": "toast", "m": f"{label}: {r.status}"[:34]})
        except Exception as e:
            log(f"action {label!r} for {nid} failed: {e}")
            await self.send({"t": "toast", "m": f"{label}: failed"[:34]})

    async def publish_claude_summary(self):
        """Throttled tier-2 list payload to ntfy for hub-connected panels."""
        topic = self.cfg.get("ntfy_claude_topic") or ""
        url = self.cfg.get("ntfy_url") or ""
        if not topic or not url:
            return
        nowt = time.time()
        if nowt - self.last_ntfy_pub < 10:
            return
        self.last_ntfy_pub = nowt
        items = []
        for s in list(self.sessions.values())[:6]:
            val = f"{s.get('ctx', 0)}%"
            if s.get("cost"):
                val += f" ${s['cost']:.2f}"
            items.append({"label": f"{s.get('title', '?')} {s.get('state', '')}"[:17],
                          "value": val})
        body = json.dumps({"view": "list", "title": "claude sessions",
                           "items": items, "ttl": 120})
        headers = {}
        if self.cfg.get("ntfy_token"):
            headers["Authorization"] = f"Bearer {self.cfg['ntfy_token']}"
        try:
            async with self.http().post(f"{url}/{topic}", data=body,
                                        headers=headers) as r:
                if r.status != 200:
                    log(f"ntfy publish failed: HTTP {r.status}")
        except Exception as e:
            log(f"ntfy publish failed: {e}")

    # ---- HTTP handlers ----

    async def h_notify(self, req):
        """Local notification intake — same schema the hub sends devices."""
        try:
            d = await req.json()
        except Exception:
            return web.json_response({"error": "invalid json"}, status=400)
        if not isinstance(d, dict):
            return web.json_response({"error": "body must be an object"}, status=400)
        try:
            priority = int(d.get("priority", 3))
            ttl = int(d.get("ttl", 1800))
        except (TypeError, ValueError):
            return web.json_response(
                {"error": "priority/ttl must be integers"}, status=400)
        acts = d.get("actions") or []
        if not isinstance(acts, list) or any(not isinstance(a, dict) for a in acts):
            return web.json_response(
                {"error": "actions must be a list of objects"}, status=400)
        self.notif_seq += 1
        nid = str(d.get("id") or f"local-{self.notif_seq}")
        msg = {"t": "notify", "id": nid,
               "topic": str(d.get("topic", "local"))[:20],
               "view": d.get("view", "card"),
               "priority": priority,
               "ttl": ttl}
        if "message" in d and "body" not in d:
            msg["body"] = d["message"]
        for k in ("title", "body", "icon", "color", "unit", "label",
                  "value", "max", "items", "points"):
            if k in d:
                msg[k] = d[k]
        if acts:
            msg["actions"] = [{"label": str(a.get("label", "?"))[:15]} for a in acts[:3]]
            self.notif_actions[nid] = (acts[:3], time.time() + ttl + 60)
        await self.send(msg)
        return web.json_response({"ok": self.writer is not None, "id": nid})

    async def h_statusline(self, req):
        d = await req.json()
        sid = d.get("session_id") or ""
        if not sid:
            return web.Response(status=204)
        s = self.sess(sid)
        ws_ = d.get("workspace") or {}
        cwd = ws_.get("current_dir") or d.get("cwd") or ""
        if cwd:
            s["title"] = os.path.basename(cwd.rstrip("/"))[:20]
        model = d.get("model") or {}
        if isinstance(model, dict) and model.get("display_name"):
            s["model"] = model["display_name"][:16]
        cost = d.get("cost") or {}
        if isinstance(cost, dict) and cost.get("total_cost_usd") is not None:
            try:
                s["cost"] = round(float(cost["total_cost_usd"]), 2)
            except (TypeError, ValueError):
                pass
        s["ctx"] = extract_ctx_pct(d, s["ctx"])
        s["last"] = time.time()
        await self.push_session(sid)
        await self.publish_claude_summary()
        return web.Response(status=204)

    async def h_hook(self, req):
        event = req.match_info["event"]
        d = await req.json()
        hook = d.get("hook") or {}
        sid = hook.get("session_id") or ""
        if not sid:
            return web.Response(status=204)

        if event == "SessionEnd":
            self.sessions.pop(sid, None)
            await self.send({"t": "remove", "sid": sid})
            return web.Response(status=204)

        s = self.sess(sid)
        if d.get("tmux_pane"):
            s["pane"] = d["tmux_pane"]
        cwd = hook.get("cwd") or ""
        if cwd and s["title"] == "?":
            s["title"] = os.path.basename(cwd.rstrip("/"))[:20]

        if event == "UserPromptSubmit":
            s["state"], s["tool"] = "working", ""
        elif event == "PreToolUse":
            s["state"] = "working"
            tool = hook.get("tool_name", "?")
            summary = tool_summary(tool, hook.get("tool_input"))
            s["tool"] = f"{tool}: {summary}"[:62] if summary else tool
        elif event == "Stop":
            s["state"], s["tool"] = "idle", ""
        elif event == "Notification":
            msg = hook.get("message") or ""
            s["state"] = "perm" if "permission" in msg.lower() else "notify"
            s["tool"] = msg[:62]
        elif event == "SessionStart":
            s["state"] = "idle"

        s["last"] = time.time()
        await self.push_session(sid)
        return web.Response(status=204)

    async def h_permission(self, req):
        d = await req.json()
        sid = d.get("session_id") or ""
        tool = d.get("tool_name") or "?"
        detail = tool_summary(tool, d.get("tool_input")) or tool
        if d.get("tmux_pane") and sid:
            self.sess(sid)["pane"] = d["tmux_pane"]

        if self.writer is None:
            return web.json_response({"decision": "pass"})

        pid = uuid.uuid4().hex
        timeout = int(self.cfg["perm_timeout_s"])
        async with self.perm_lock:  # one dialog on the device at a time
            fut = asyncio.get_event_loop().create_future()
            self.pending[pid] = fut
            prev_state = None
            if sid:
                s = self.sess(sid)
                prev_state, s["state"] = s["state"], "perm"
                await self.push_session(sid)
            await self.send(
                {"t": "perm", "id": pid, "sid": sid, "tool": tool,
                 "detail": detail[:105], "timeout": timeout}
            )
            log(f"permission ask: {tool} — {detail[:60]!r}")
            try:
                decision = await asyncio.wait_for(fut, timeout)
            except asyncio.TimeoutError:
                decision = "pass"
                await self.send({"t": "perm_cancel", "id": pid})
            finally:
                self.pending.pop(pid, None)
            if sid and prev_state is not None:
                s = self.sess(sid)
                s["state"] = prev_state
                await self.push_session(sid)
        log(f"permission answer: {decision}")
        return web.json_response({"decision": decision})

    async def h_health(self, req):
        return web.json_response(
            {"device_connected": self.writer is not None,
             "sessions": len(self.sessions),
             "tmux": TMUX_BIN is not None}
        )

    # ---- housekeeping ----

    async def prune_task(self):
        while True:
            await asyncio.sleep(60)
            now = time.time()
            for sid in list(self.sessions):
                if now - self.sessions[sid]["last"] > SESSION_TTL_S:
                    self.sessions.pop(sid, None)
                    await self.send({"t": "remove", "sid": sid})
            for nid, (_acts, exp) in list(self.notif_actions.items()):
                if now > exp:
                    self.notif_actions.pop(nid, None)


def extract_ctx_pct(d, fallback):
    """Context-window usage % from the statusline context_window block."""
    cw = d.get("context_window")
    if isinstance(cw, dict):
        pct = cw.get("used_percentage")
        if isinstance(pct, (int, float)):
            return max(0, min(100, int(pct)))
        size = cw.get("context_window_size") or 200000
        usage = cw.get("current_usage") or {}
        if isinstance(usage, dict):
            total = sum(
                int(usage.get(k) or 0)
                for k in ("input_tokens", "cache_read_input_tokens",
                          "cache_creation_input_tokens")
            )
            if total and size:
                return max(0, min(100, int(total * 100 / size)))
    return fallback


def main():
    cfg = load_config()
    bridge = Bridge(cfg)

    app = web.Application()
    app.router.add_post("/statusline", bridge.h_statusline)
    app.router.add_post("/hook/{event}", bridge.h_hook)
    app.router.add_post("/permission", bridge.h_permission)
    app.router.add_post("/notify", bridge.h_notify)
    app.router.add_get("/health", bridge.h_health)

    async def start():
        runner = web.AppRunner(app)
        await runner.setup()
        site = web.TCPSite(runner, "127.0.0.1", cfg["http_port"])
        await site.start()
        log(f"http on 127.0.0.1:{cfg['http_port']}")
        await asyncio.gather(bridge.device_server(), bridge.prune_task())

    try:
        asyncio.run(start())
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
