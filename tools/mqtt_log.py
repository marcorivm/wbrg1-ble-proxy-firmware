#!/usr/bin/env python3
"""Tail the WBRG1's MQTT log/status/telemetry topics with timestamps.
  usage: mqtt_log.py [seconds=30] [extra-topic ...]
Optionally publish a command first with: mqtt_log.py <secs> --cmd "conn 582abd7d3152"
"""
import sys, re, os, time, paho.mqtt.client as m

cfg = open(os.path.join(os.path.dirname(__file__), "..", "sketches", "wbrg1_ble_proxy", "config.h")).read()
def d(k): return re.search(r'#define\s+%s\s+"([^"]*)"' % k, cfg).group(1)
base = "wbrg1/" + d("SCANNER_ID") + "/"
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 30
cmd = None
args = sys.argv[2:]
if "--cmd" in args:
    i = args.index("--cmd"); cmd = args[i + 1]; args = args[:i] + args[i + 2:]
topics = [base + t for t in ("log", "status", "telemetry")] + args

t0 = time.time()
def on_msg(c, u, msg):
    print(f"{time.time()-t0:6.1f}s {msg.topic.split('/')[-1]:<9} {msg.payload.decode('latin1')}", flush=True)

c = m.Client(m.CallbackAPIVersion.VERSION2) if hasattr(m, "CallbackAPIVersion") else m.Client()
c.username_pw_set(d("MQTT_USER"), d("MQTT_PASS"))
c.on_message = on_msg
c.connect(d("MQTT_HOST"), 1883, 10)
for t in topics: c.subscribe(t)
c.loop_start()
if cmd:
    time.sleep(0.5)
    c.publish(base + "cmd", cmd, qos=1).wait_for_publish(5)
    print(f"{time.time()-t0:6.1f}s >>> cmd: {cmd}", flush=True)
time.sleep(secs)
c.loop_stop(); c.disconnect()
