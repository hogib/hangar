# msg/bridge_send_req.msg: bridge.send (docs/08-bridge.md "Files"): a file of this
# machine to the other side of the tether. The caller names no path: the file is
# <name> in the bridge's outbox, and lands under the same name where the other
# side keeps the files of <dest>.
char dest[33]       # a [Files <dest>] section of bridge.conf that goes the other way
char name[65]       # the file's name in the outbox: letters, digits, '.', '_', '-'; no leading '.'
char reserved[6]
