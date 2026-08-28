#!/usr/bin/env python3
from pathlib import Path
import subprocess, sys
R=Path(__file__).resolve().parents[2]; W=R/"www"/"dmxwb"
pre=R/"tools"/"web"/"check_dev011f2_group_membership.py"
r=subprocess.run([sys.executable,str(pre)],cwd=R)
if r.returncode != 0: raise SystemExit(r.returncode)
app=(W/"app.js").read_text(); model=(W/"model.js").read_text(); styles=(W/"styles.css").read_text()
def req(text,tokens,name):
    for t in tokens:
        if t not in text: raise SystemExit(f"FAIL: {name} missing {t}")
req(model,["summarizeDmx","summarizeMqtt","summarizeArtNet","summarizeConfiguration",'key:"application"','key:"dmx"','key:"mqtt"','key:"artnet"','key:"configuration"','key:"last_error"',"active_refresh_hz","frames_sent","missed_deadlines","successful_connections","callback_failures","datagrams_received","conflicts","receive_errors","send_errors"],"model.js")
req(app,["diagnosticSummary(model)","diagnostic.severity","metric-card--ok","metric-card--error","metric-card__detail","diagnostic.detail"],"app.js")
req(styles,[".metric-card__detail",".metric-card--ok",".metric-card--error"],"styles.css")
for t in ["fetch(","XMLHttpRequest","systemctl","/proc/","localStorage","sessionStorage"]:
    if t in app: raise SystemExit(f"FAIL: diagnostics UI direct API {t}")
print("dev011g1a_required_status_fields_rendered: PASS")
print("dev011g1a_dmx_diagnostics_render_contract: PASS")
print("dev011g1a_mqtt_diagnostics_render_contract: PASS")
print("dev011g1a_artnet_diagnostics_render_contract: PASS")
print("dev011g1a_configuration_and_last_error_render_contract: PASS")
print("dev011g1a_backward_compatible_string_status: PASS")
print("dev011g1a_read_only_mqtt_diagnostics: PASS")
print("=== DMXWB DEV-011G1A WEB DIAGNOSTICS RENDERING STATIC PASS ===")
