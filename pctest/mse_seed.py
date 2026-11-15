# Seeds pctest's MSE test file with python-libtorrent, encryption FORCED:
# libtorrent refuses plaintext connections, so a peer only gets data through
# MSE/RC4.  Used by test_mse_live.exe (see pctest/run_mse_live.ps1).
#
#   python mse_seed.py <dir> <port> <seconds> [forced|enabled]
import sys
import time

import libtorrent as lt

d, port, secs = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
mode = sys.argv[4] if len(sys.argv) > 4 else "forced"
policy = lt.enc_policy.forced if mode == "forced" else lt.enc_policy.enabled

ses = lt.session({
    "listen_interfaces": "127.0.0.1:%d" % port,
    "enable_dht": False, "enable_lsd": False, "enable_upnp": False, "enable_natpmp": False,
    "out_enc_policy": policy, "in_enc_policy": policy,
    "allowed_enc_level": lt.enc_level.rc4,   # RC4 only: the full MSE path
    "prefer_rc4": True,
    "allow_multiple_connections_per_ip": True,
    "alert_mask": lt.alert_category.error | lt.alert_category.peer | lt.alert_category.connect,
})
ti = lt.torrent_info(d + "/mse_test.torrent")
atp = lt.add_torrent_params()
atp.ti = ti
atp.save_path = d
atp.flags |= lt.torrent_flags.seed_mode
h = ses.add_torrent(atp)
print("seeding %s on 127.0.0.1:%d, encryption=%s, libtorrent %s"
      % (ti.name(), port, mode, lt.__version__), flush=True)

end = time.time() + secs
uploaded = 0
while time.time() < end:
    for a in ses.pop_alerts():
        if isinstance(a, (lt.peer_connect_alert, lt.peer_disconnected_alert, lt.peer_error_alert,
                          lt.incoming_connection_alert)):
            print("alert:", a.message(), flush=True)
    st = h.status()
    if st.total_upload != uploaded:
        uploaded = st.total_upload
    time.sleep(0.2)
print("uploaded %.1f MB" % (uploaded / 1048576.0), flush=True)
