# denim — Unix shell utilities

Shell tools organized into useful **screw** and **toys**.

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
| `echofile` | Create files named after their content |
| `iwatch` | Run command on file change (inotify) |
| `iiwatch` | Run command on file change, F5, or midnight |
| `killtree` | Kill a process and all its descendants |
| `mkdire` | mkdir parent dirs, then execute command |
| `mkpin` | Name a directory, open files from anywhere with tab completion |
| `mutexec` | Clean wrapper around &>/dev/null |
| `nugget` | PKGBUILD-style downloader (URLs, archives, VCS) |
| `pdsc` | List process descendants |
| `rssh` | Sync directory to remote host, run commands |
| `slay` | Kill processes with SIGKILL |
| `soak` | Read all stdin, then write to file |
| `tabexec` | Execute commands, format output as aligned table |
| `vicat` | Syntax highlight any file using vim's engine |
| `withd` | Run command in directory with colored status |
| `zenexec` | Clean wrapper around 2>/dev/null |
| `zrun` | Run commands from compressed scripts |

Shell scripts: `path-expand.sh`, `path-shorten.sh`
(C source alongside in screws/)

C source (no script): `prefix.c`

## toys/

| Tool | Purpose |
|------|---------|
| `memento-mori-cal` | Visualize your life in weeks (ASCII grid) |
| `worth-the-time-xkcd-1205` | Calculate if automation saves time |
| `spell-number` | Convert integers to words (`-l en|de|fr|ru`) |
| `spell-topic` | Multilingual vocabulary cheat sheet |
