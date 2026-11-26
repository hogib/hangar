# msg/bridge_link.msg: bridge.<role>.link, the tether link's state (docs/08-bridge.md
# "Link status"), at 10 Hz and on change, whether the link is up or not.
u8   state            # 0 off, 1 searching, 2 connecting, 3 up, 4 ambiguous, 5 refused
u8   config_match     # 1 when the two sides run the same release, msg/ tree and bridge.conf
u8   peer_role        # 0 unknown, 1 vehicle, 2 topside
u8   reserved0
u32  peer_id          # the peer bridge's random id, drawn at its start
u8   peer_boot_id[16] # the other machine's boot id, from the handshake
char peer_addr[48]    # address:port the link uses
u64  last_rx_ns       # CLOCK_MONOTONIC: the last datagram whose tag and packet_seq checked
u64  rtt_ns           # the last ping's round trip
u64  rtt_min_ns       # the smallest round trip of the last 10 s
u64  loss_rx          # datagrams of the peer lost on the way here, from packet_seq
u64  rejected         # datagrams refused: a tag that does not check, a replay, malformed
i64  clock_offset_ns  # the peer's CLOCK_MONOTONIC minus ours, the estimate in force
u64  clock_uncertainty_ns # half the kept sample's round trip plus its age times 500 ppm
f32  clock_skew_ppm   # 0: not estimated in this stage
u32  topics_rx        # mirrored topics received and published here
u64  msgs_tx          # mirrored messages sent
u64  msgs_rx          # mirrored messages published here
u64  retransmits      # reliable datagrams sent again
u64  clamped          # translated stamps clamped to keep a topic's stamps rising
u64  capped           # translated stamps held to the time their message came: none is after its arrival
u64  dropped_old      # received messages whose seq was not above the last published
u64  dropped_late     # command messages older than the mirror's MaxAge on arrival
char detail[96]       # one line: why, or what; the two versions when they differ
