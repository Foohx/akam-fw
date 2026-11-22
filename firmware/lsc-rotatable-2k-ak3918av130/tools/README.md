# Host-side helpers (lsc-rotatable-2k-ak3918av130)

Host-side tools of this model (ONVIF probe, PTZ and DP clients, ...). Nothing here runs on the camera.

- `onvif_events_probe.py`: read-only probe of the stock ONVIF event service (capabilities, announced topics,
  PullPoint refusal, and a push `Subscribe` listener that prints the Notify messages). Python standard library only;
  host, port and credentials are arguments / `ONVIF_PASSWORD`. Usage and meaning of the results:
  [npu-findings](../../../docs/porting/lsc-rotatable-2k/npu-findings.md). Test: `tests/rot2k_onvif_probe_test.py`.
- `mqtt_sink.py`: minimal MQTT 3.1.1 broker sink for testing the app's MQTT output without a real broker (prints every
  publish and will, never forwards or stores anything, never prints a password). Python standard library only.
  `python3 mqtt_sink.py --bind <LAN address> --port 1883`. Test: `tests/rot2k_mqtt_sink_test.py`.

- `flash_appfs.sh`: write + verify + rollback of the card-less appfs image to mtd3. **Writes flash**; dry run by default,
  live only with `AKAM_I_HAVE_RECOVERY=yes` and `--execute`. See
  [cardless-appfs](../../../docs/porting/lsc-rotatable-2k/cardless-appfs.md). Image built by `../appfs/build_appfs.sh`.

The Phase 1 validation otherwise uses `ffprobe`, `ssh` and `curl`
([checklist](../../../docs/archive/rot2k-phase1-validation.md)).
