#!/usr/bin/env python3
"""Publish the MQTT 'ota' command to the WBRG1.
  usage: ota_trigger.py <http-host> [port=8000] [resource=/wbrg1_ota.bin]
Broker/creds/topic are read from sketches/wbrg1_ble_proxy/config.h.
"""
import sys, re, os, paho.mqtt.client as m

cfg = open(os.path.join(os.path.dirname(__file__), "..", "sketches", "wbrg1_ble_proxy", "config.h")).read()
def d(k): return re.search(r'#define\s+%s\s+"([^"]*)"' % k, cfg).group(1)
host = sys.argv[1]; port = sys.argv[2] if len(sys.argv) > 2 else "8000"
res = sys.argv[3] if len(sys.argv) > 3 else "/wbrg1_ota.bin"
topic = "wbrg1/" + d("SCANNER_ID") + "/cmd"
c = m.Client(m.CallbackAPIVersion.VERSION2) if hasattr(m, "CallbackAPIVersion") else m.Client()
c.username_pw_set(d("MQTT_USER"), d("MQTT_PASS"))
c.connect(d("MQTT_HOST"), 1883, 10)
msg = f"ota {host} {port} {res}"
r = c.publish(topic, msg, qos=1); r.wait_for_publish(5)
print("published to", topic, ":", msg)
c.disconnect()
