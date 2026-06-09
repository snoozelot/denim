# denim

Unix shell utilities. Two categories:

**screws/** — production tools for everyday shell work

| Tool | Purpose |
|------|---------|
| `acronic` | Run cronic asynchronously, fully detached |
| `arc` | Universal archive wrapper (tar, zip, 7z, rar, deb, rpm, cbz...) |
| `ched` | Cache command output with time-based expiration |
| `cidr-contains` | Check if one CIDR block contains another |
| `cidr-host` | Calculate host IP within CIDR prefix (Tofu/Terraform compatible) |
| `cidr-subnet` | Calculate subnet within CIDR prefix (Tofu/Terraform compatible) |
| `cpstamped` | Copy files with Unix timestamp suffix |
| `cronic` | Silent cron runner — output only on failure |
| `cycle` | Cycle through ring of options |
| `cyclines` | Permute lines via cycle notation |
| `echofile` | Create files named after their content |
| `iwatch` | Run command on file change (inotify) |
| `iiwatch` | Run command on file change, F5, or midnight |
| `killtree` | Kill a process and all its descendants |
| `mkdire` | mkdir parent dirs, then execute command |
| `pdsc` | List process descendants |
| `slay` | Kill processes with SIGKILL |
| `tabexec` | Execute commands, format output as aligned table |
| `vicat` | Syntax highlight any file using vim's engine |
| `withd` | Run command in directory with colored status |
| `zrun` | Run commands from compressed scripts |

Shell scripts: `path-expand.sh`, `path-shorten.sh` (C source alongside in screws/)
C source (no script): `prefix.c`

**toys/** — fun and educational

| Tool | Purpose |
|------|---------|
| `memento-mori-cal` | Visualize your life in weeks (ASCII grid) |
| `worth-the-time-xkcd-1205` | Calculate if automation saves time |
| `spell-number-{en,de,fr,ru}` | Convert integers to words |
| `spell-topic` | Multilingual vocabulary cheat sheet |
