# device-nightly-reboot-window — defect (infrastructure; found by the 2026-08-13 device-tests nightly)

## 1. Definition

**1.1 Symptom.** The `wsl-nvidia` leg of run 31682002702 died 70 s into its
first test with no test failure and no log:

```
The self-hosted runner lost communication with the server. Verify the machine
is running and has a healthy network connection.
```

The uploaded `device-tests.log` stops mid-line:

```
[----------] 3 tests from AmdgpuCoopBf16Tests
[ RUN      ] AmdgpuCoopBf16Tests.bf16OperandsF32AccLowersWithoutAborting
```

**1.2 It is not the test.** `AmdgpuCoopBf16Tests.bf16OperandsF32AccLowersWithoutAborting`
was re-run on `phoenix-wsl` from the very binary the dead job had built
(`~/actions-runner/_work/cajeta/cajeta/build/test/cajeta_test`), under a 12 GB
address-space cap: **PASSED (79 520 ms)**. It is slow, not fatal.

**1.3 Root cause — Windows Update rebooted the host mid-run.** Timeline, all
`America/New_York` (the runner box, `PHOENIX`):

| time | event | source |
|---|---|---|
| 04:25 | nightly starts (cron `0 7 * * *` UTC fired ~85 min late) | run 31682002702 |
| 04:49:40 | `Run device tests` step begins | job 94389349868 |
| 04:50:50 | runner worker log stops mid-write | `_diag/Worker_20260813-082545-utc.log` |
| 04:52:21 | Event Log service stops | System event 6006 |
| 04:53:55 | `TrustedInstaller.exe` initiates restart of PHOENIX | System event 1074 |
| 20:35 | WSL runner comes back — on interactive logon | `Start-WSL-Runner` task |

**1.4 Why that hour.** PHOENIX's Windows Update Active Hours are **08:00–02:00**
local (`HKLM\SOFTWARE\Microsoft\WindowsUpdate\UX\Settings`), so 02:00–08:00 is
precisely the window in which Windows may reboot unattended. The cron
(`0 7 * * *` UTC = 03:00 local) sat inside it, and GitHub's scheduler adds
delay on top — this run fired 85 minutes late, landing at 04:25.

**1.5 Second, independent defect — the runner does not come back.** The
`Start-WSL-Runner` scheduled task that starts the WSL VM has a single
**logon** trigger (`MSFT_TaskLogonTrigger`, `PHOENIX\julian`, Interactive).
The actions runner itself is a healthy systemd unit *inside* WSL
(`actions.runner.jklappenbach-cajeta.phoenix-wsl.service`, `/etc/wsl.conf` has
`systemd=true`), so it starts fine — but only once something starts the VM,
and nothing does until a human logs in. After the 04:53 reboot the WSL runner
was offline for ~15.5 hours. Any job queued to `phoenix-wsl` in that window
would have hung to its 6 h timeout.

**1.6 Non-goal.** Chasing a phantom bug in `AmdgpuCoopBf16Tests`. §1.2 settles
that; the 79 s runtime is worth a look on its own, but it is not this defect.

## 2. Acceptance

- **2.1** The nightly's scheduled start, plus GitHub's observed scheduling
  delay (~90 min) plus a full run (~2 h), lands entirely inside PHOENIX's
  Active Hours — the window in which Windows will not reboot unattended.
- **2.2** The cron's placement is documented *in the workflow file* with the
  constraint that governs it, so a future edit does not silently move it back
  into the reboot window.
- **2.3** (host config, outside this repo) The WSL VM restarts without an
  interactive logon, so an unattended reboot costs one run rather than every
  run until someone notices. Either add a boot trigger to `Start-WSL-Runner`
  that runs whether or not the user is logged on, or widen Active Hours to
  cover the nightly and accept the reboot-recovery gap.
- **2.4** A run killed this way stays diagnosable: `device-tests.log` is
  uploaded even when truncated (it was — `if: always()` on the upload step is
  what made §1.1 readable at all). Keep it.

## 3. Resolution — 2026-08-20

**3.1 Acceptance 2.3 is MET, and it was the load-bearing one.** PHOENIX now
starts the WSL subsystem at *machine* start rather than on interactive logon.
Nothing in this repo changed; the host was doing exactly what it had been
configured to do, which was to wait for a human.

**3.2 §1.5 was not a secondary annoyance — it was the whole failure mode after
Aug 15.** Every `wsl-nvidia` attempt from Aug 16 through Aug 20 failed at
**exactly 600 s** (601/601/600/600/601), never completing `Set up job` — no
checkout, no build, no log. That constant is the tell: there is no 10-minute
timeout anywhere in the workflow (`timeout-minutes: 360`), so 600 s is
GitHub's grace period for a job assigned to a runner that never reports. The
VM was simply down — after each host restart it stayed down until someone
logged in — while GitHub still held a registration for `phoenix-wsl` and kept
handing it work. The jobs were never running and dying; they were never
running at all.

This is why the §1.3 reboot diagnosis, though correct for the 2026-08-13 run,
did not stop the bleeding: moving the cron into Active Hours prevents the
*reboot*, but any reboot at all (or any shutdown) left the runner unstartable,
and Aug 16 failed at 16:06 local — nowhere near the reboot window.

**3.3 Verified green.** With the boot trigger in place, `wsl-nvidia` passed
twice on 2026-08-20: standalone (run 32412895281, 65 tests on hardware, 0
failures) and again under the nightly's own wider filter with both legs
running concurrently on PHOENIX (run 32416638186). The concurrent pass also
retires the hypothesis that the two legs contend fatally for the box.

**3.4 Coverage gap noted, not a defect.** On the same 4090, the WSL leg
executes **65** tests against the Windows leg's **118** — the Vulkan and
several vendor suites gate themselves off inside the VM. Both legs report
green, so this is exactly the silent-skip blind spot the workflow exists to
surface (see its header comment). Worth a look on its own; it is not this
defect.
