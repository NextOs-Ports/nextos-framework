# Owner runtime (nxbootstrap 0.8.0, V5 mission 7A.1/7A.2)

Roles and PATHS are separated; no owner file shares a path with a member that unzip/update writes.

| path | class | who writes | healed? | in generation hash set? |
|---|---|---|---|---|
| `adapter-env.sh` | `sealed-runtime` | package (generation member, role `runtime-hook`) | yes | yes |
| `defaults/port-env.sh` | package seed | package | yes | yes |
| `defaults/NEXTOSSETTINGS.txt` | package seed (`NEXTOS_SETTINGS/2`) | package | yes | yes |
| `defaults/NEXTOSCONTROLLERS.gptk` | package seed | package | yes | yes |
| `<port>/port-env.sh` | `owner-seeded` live hook | owner | **never** | **never** |
| `<port>/NEXTOSSETTINGS.txt` | `owner-seeded` typed settings | owner | never | never |
| `<port>/NEXTOSCONTROLLERS.gptk` | `owner-seeded` controls | owner | never | never |
| `<port>/game/<native config>` | `owner-native` (declared by the port) | owner in `ENGINE`; projected sink in `NEXTOS`; CAS participant in `SYNCHRONIZED` | never | never |

Order (observable, printed once as `NX-OWNER-RUNTIME/1 order=...`):

```
authenticate/heal only the sealed artifact_generation
 -> NXExtract (data gate)
 -> NXSplash (outside the reach of owner options)
 -> seed absent live owners from defaults/ (atomic, set -C, never through a symlink)
    untouched copy + new default = migrate with receipt; edited copy = <file>.new
 -> source adapter-env.sh (sealed)      [bash -n first; reserved-variable guard]
 -> source port-env.sh (owner)          [bash -n first; reserved-variable guard]
 -> launch
```

Failure policy of the hook (shell, trusted to the owner): a hook that does not parse aborts early
and visibly with bash's own `file: line N:` diagnostic; a hook that changes a reserved variable
aborts visibly naming the variable. Neither path rewrites the owner's bytes ("delete it to reseed
from defaults/" is the documented reset). Typed settings (`NEXTOSSETTINGS.txt`) follow the port's
`video.invalid_policy` (`fail_closed` | `last_known_good` | `package_default`) inside the adapter,
which reads the bytes with `nxcompat_settings_parse2` and reports `NEXTOSSETTINGS.txt:<line>: <reason>`.

Opt-in: `"owner_runtime": "1"` in `nxport.json` (schema v3). Without it the V4 rendering is
unchanged. With it the generator refuses `port-env.sh` as a generation member, a required file or
the prepare script; the sealed hook must be `adapter-env.sh`.
