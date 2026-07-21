"""PlatformIO pre-script: generate src/wifi_secrets.h from .env.

Header generation (instead of -D flags) keeps passwords with special
characters off the compiler command line. No .env, or a CHANGE_ME
placeholder, means no header — firmware falls back to the captive portal.
"""
import os

Import("env")

project_dir = env.subst("$PROJECT_DIR")
env_path = os.path.join(project_dir, ".env")
header_path = os.path.join(project_dir, "src", "wifi_secrets.h")


def c_escape(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


vals = {}
if os.path.exists(env_path):
    with open(env_path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            k, v = line.split("=", 1)
            vals[k.strip()] = v.strip().strip('"').strip("'")

ssid = vals.get("WIFI_SSID", "")
pw = vals.get("WIFI_PASS", "")

if ssid and pw and pw != "CHANGE_ME":
    with open(header_path, "w") as f:
        f.write("#pragma once\n// generated from .env by env_flags.py — do not commit\n")
        f.write('#define WIFI_SSID "%s"\n' % c_escape(ssid))
        f.write('#define WIFI_PASS "%s"\n' % c_escape(pw))
        if vals.get("BRIDGE_HOST"):
            f.write('#define BRIDGE_HOST "%s"\n' % c_escape(vals["BRIDGE_HOST"]))
        if vals.get("BRIDGE_PORT"):
            f.write("#define BRIDGE_PORT %s\n" % int(vals["BRIDGE_PORT"]))
    print("wifi_secrets.h generated for SSID '%s'" % ssid)
else:
    if os.path.exists(header_path):
        os.remove(header_path)
    print("no usable .env — captive portal build")
