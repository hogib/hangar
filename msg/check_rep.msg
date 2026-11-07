# msg/check_rep.msg: safety.check's reply: the arming checklist, run without
# arming (docs/07-safety.md, Vehicle states)
u8   ok            # 1: arming would be accepted now
u8   reserved[7]
char detail[240]   # what would fail, one clause per check, blank when ok
