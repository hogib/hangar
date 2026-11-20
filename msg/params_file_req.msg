# msg/params_file_req.msg: the request of mavlink.params_dump and mavlink.params_load:
# a file in the module's parameter backup directory (BackupDir= in mavlink.conf), by
# name; letters, digits, '.', '-' and '_' only, no path
char name[64]    # without the .params suffix
