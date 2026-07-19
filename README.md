# denim — Unix shell utilities

Shell tools organized into useful **screws**, and **toys**.

## screws/ — production tools

| Tool | Purpose |
|------|---------|
| `acronic` | Run in background, log output, show only on failure |
| `arc` | Universal archive wrapper (tar, zip, 7z, rar, deb, rpm, cbz...) |
| `ched` | Cache command output with time-based expiration |
| `cidr-contains` | Check if one CIDR block contains another |
| `cidr-host` | Calculate host IP within CIDR prefix (Tofu/Terraform compatible) |
| `cidr-subnet` | Calculate subnet within CIDR prefix (Tofu/Terraform compatible) |
| `cpstamped` | Copy files with Unix timestamp suffix |
| `cdexec` | Chain-cd through directories, exec last argument |
| `cronic` | Run a command, log output, show only on failure |
| `cycle` | Return next item in a list, wrapping at end |
| `cyclines` | Reorder lines by positional swaps |
| `dlnactl` | Browse DLNA/UPnP media servers (discovery, browse, search) |
| `echofile` | Create files named after their content |
| `iwatch` | Run command on file change (inotify) |
| `iiwatch` | Run command on file change, F5, or midnight |
| `killtree` | Kill a process and all its descendants |
| `mkdire` | mkdir parent dirs, then execute command |
| `mkpin` | Name a directory, open files from anywhere with tab completion |
| `mutexec` | Clean wrapper around &>/dev/null |
| `nugget` | PKGBUILD-style downloader (URLs, archives, VCS) |
| `pdsc` | List process descendants |
| `playerctl-my` | MPRIS media player control via D-Bus (no glib deps) |
| `pup.js` | CSS selector HTML filter (stdin→selector→html/text/attr/json) |
| `realpath-my` | Pure bash realpath(1) replacement (no coreutils) |
| `rssh` | Sync directory to remote host, run commands |
| `slay` | Kill processes with SIGKILL |
| `soak` | Read all stdin, then write to file |
| `tabexec` | Execute commands, format output as aligned table |
| `vicat` | Syntax highlight any file using vim's engine |
| `withd` | Run command in directory with colored status |
| `xrm` | Move files to dated trash (~/.limbo/YYYY-MM-DD/) |
| `yq.js` | XML/HTML/YAML converter through jq (xq/hq multi-name) |
| `zenexec` | Clean wrapper around 2>/dev/null |
| `zrun` | Run commands from compressed scripts |

C source (with bash alternatives): `prefix.c`, `path-expand.c`/`.sh`, `path-shorten.c`/`.sh`

## toys/

| Tool | Purpose |
|------|---------|
| `memento-mori-cal` | Visualize your life in weeks (ASCII grid) |
| `proquint` | Encode/decode numbers as pronounceable identifiers |
| `worth-the-time-xkcd-1205` | Calculate if automation saves time |
| `spell-number` | Convert integers to words (`-l en\|de\|fr\|ru`) |
| `spell-topic` | Multilingual vocabulary cheat sheet |

## ssam/ — structural regex stream editor

| File | Purpose |
|------|---------|
| `ssam.c` | Sam-language text editor (dot-based, x/g/v/y commands) |
| `ssam-regex.h` | Thompson NFA regex engine (O(nm), no backtracking) |

Standalone single-file binaries via ccraft shebang.  Test suite in `ssam.t`.
