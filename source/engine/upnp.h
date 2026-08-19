#ifndef TSNX_UPNP_H
#define TSNX_UPNP_H

// Minimal UPnP IGD client: SSDP M-SEARCH -> device description -> SOAP
// AddPortMapping, so firewalled/NAT'd seeders can dial IN through the home
// router. Runs on its own thread with a hard time budget; failing (no IGD,
// mapping refused, network trouble) is silent and non-fatal -- the engine
// simply keeps working outbound-only.
//
// On success the external port is fed to torrent_set_announce_port(), so
// trackers and the DHT advertise a dial-back address that actually reaches
// the console's listener.
//
// Returns 0 if the probe thread was started, -1 otherwise.
int upnp_start(int internal_port);

// Marks the probe as cancelled (the thread exits once its current step ends).
void upnp_stop(void);

#endif
