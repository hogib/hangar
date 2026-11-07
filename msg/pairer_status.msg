# msg/pairer_status.msg: pairer.status, one message per dataset and part,
# a round every second (docs/06-recorder.md, Live: the pairer)
char dataset[32]
char part[48]        # the part's name; "" on the dataset's own line
u8   index           # position in this round
u8   count           # messages in this round
u8   reserved[6]
u64  rows            # rows published on dataset.<name>
u64  dropped         # rows removed by Where with Filter=drop, or by Missing=drop
u64  masked          # rows published with the masked flag
u64  misses          # this part: joins that found no message
u64  latency_ns      # this part: the largest latency measured, from publication to the join
u32  slots           # this part's ring depth
u32  needed_slots    # the depth the measured latency needs: (Slots - 1) / rate above it
