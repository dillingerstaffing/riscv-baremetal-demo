<!-- PROOF-HEADER
Checks: 3000
Mismatches: 0
Environment: QEMU 8.2.2
Verdict: PASS
-->

# Proof: mtimecmp delivery accuracy (backlog item 36)

## What was built

`src/mtimecmp/`: a bare-metal RISC-V program that programs the CLINT
`mtimecmp` register 5000 mtime ticks (500 us at the 10 MHz timebase)
ahead of the current `mtime`, then measures the actual timer-trap
delivery: the trap entry stamps `rdtime` and `rdcycle`, and the
experiment publishes the distribution of the delivery offset
`t_entry - mtimecmp` over 1000 trials. Five files, about 700 lines
total, sharing only `src/boot.S`, `src/uart.c`, and
`src/preempt/clint.c` with the other demos.

- `mt_main.c`: UART bring-up, trap vector install, in-module clock
  calibration, the exact trial asm block, the trial loop with arm
  retries, statistics (min/p50/p99/max/mean, 1-tick histogram), and the
  in-program verification checks with the PASS/FAIL verdict.
- `mt_trap.S`: M-mode trap entry. Stamps `rdtime` as the 3rd
  instruction and `rdcycle` as the 5th, saves all registers, records
  mcause/mepc, runs the C handler on a dedicated trap stack, restores
  everything, `mret` returns into the trial's spin loop.
- `mt.h`: constants, the trap save-area layout (must match the asm
  offsets), and the per-trial record.
- `PROOF.md` (this file), `bench-logs/` with the build log, three raw
  QEMU run logs, and the host CPU info.

Build: `make mtimecmp.elf CROSS=riscv-none-elf-` (xPack GCC 15.2.0,
added to `all` in the Makefile; `run-mtimecmp` target added too).
Run: `qemu-system-riscv64 -machine virt -nographic -bios none -kernel
mtimecmp.elf` (QEMU 8.2.2; the program parks in `wfi` after printing
`done`, so the logs below were taken under `timeout 600`).

## Method

Each trial arms `mtimecmp = mtime + 5000` ticks with only the machine
timer interrupt enabled in `mie`, then runs one exact asm block,
emitted verbatim:

```
rdtime  t_pre        # tick stamp before enabling interrupts
rdcycle c_pre        # rdcycle stamp
csrsi   mstatus, 8   # MIE = 1
mt_loop:             # global label
lw      t0, 0(flag)  # spin on mt_flag (set by the handler)
beqz    t0, mt_loop
mt_done:             # global label
csrci   mstatus, 8   # MIE = 0
```

The trap entry (`mt_trap.S`) stamps `rdtime`/`rdcycle` as its 3rd and
5th instructions, saves x1-x31, and calls the C handler. The handler
requires `mcause == 0x8000000000000007` (machine timer interrupt),
records the stamps into the trial record, disarms the timer
(`mtimecmp = ~0` so `mret` does not re-trap), sets `mt_flag = 1`, and
`mret` returns into the spin loop, which sees the flag and exits.

The headline metric per trial is `offset_ticks = t_entry - cmp`, the
number of mtime ticks by which actual trap delivery overshoots the
programmed value. `offset_cyc = offset_ticks * cal_ratio` is the same
quantity in cycle units, scaled by the rdcycle-per-tick ratio measured
in-module during calibration; the two counters are never mixed
directly. A second in-module measurement, the slope of
`(c_entry - c_pre)` against `(t_entry - t_pre)` summed across the
trials, cross-checks that ratio.

QEMU's timer is host-scheduled, so the run defends against host noise
the same way the wfi-latency module does: if the host deschedules the
vcpu inside the arm window (`t_pre >= cmp`) or during the wait
(`offset > 1000` ticks), the trial is re-armed and re-run, up to 100
attempts. Only clean trials are recorded; the retry count is reported
and the expected trap count is adjusted. The reported distribution
therefore describes the emulator's clean timer delivery; the retry
count quantifies how often host scheduling interfered.

In-program checks (any failure prints FAIL and the run reports FAIL):

- every attempt produced exactly one trap (`trap_count == attempts`),
  every trap with `mcause` = machine timer interrupt, and `mepc`
  inside the `[mt_loop, mt_done)` spin loop;
- `t_pre < cmp`, `t_entry >= cmp`, `t_entry >= t_pre`,
  `c_entry > c_pre` on every recorded trial (stamp ordering);
- `offset <= 1000` ticks on every recorded trial (the retry gate,
  kept as an invariant);
- calibration ratio and in-trial slope both inside 145..155;
- 64 back-to-back (rdcycle, mtime) pairs monotonic.

## Calibration results (from the run logs)

On every boot the module reads (rdcycle, mtime) twice across a
10,000,000-tick spin:

- run 1: delta 1,497,838,305, ratio 149
- run 2: delta 1,498,714,590, ratio 149
- run 3: delta 1,497,828,885, ratio 149

The in-trial slope cross-check measured 149 on all three runs. As the
wfi-latency module established on this host, `rdcycle` follows the host
CPU clock (1497.749 MHz per `/proc/cpuinfo`, see
`bench-logs/host-cpu.txt`) while `mtime` follows the 10 MHz virtual
timebase, so one rdcycle unit is one host CPU cycle, not one
nanosecond. The tick-domain metrics do not depend on this ratio; the
cycle-unit metrics are the tick metrics scaled by the measured 149.

## Results (from the run logs)

1000 trials per run, first 8 discarded from statistics (n=992):

| run | offset_ticks min/p50/p99/max/mean | offset_cyc (x149) min/p50/p99/max | retries | slope | verdict |
|-----|-----------------------------------|----------------------------------|---------|-------|---------|
| 1   | 105 / 195 / 881 / 996 / 236       | 15645 / 29055 / 131269 / 148404  | 109     | 149   | PASS |
| 2   | 102 / 198 / 822 / 998 / 248       | 15198 / 29502 / 122478 / 148702  | 252     | 149   | PASS |
| 3   | 115 / 152 / 814 / 992 / 203       | 17135 / 22648 / 121286 / 147808  | 85      | 149   | PASS |

Every claim below traces to the logs pasted at the end:

- All 3000 trials delivered exactly one timer interrupt:
  `trap_count` equaled `attempts` on every run (1109, 1252, 1085),
  every trap had `mcause = 0x8000000000000007`, and `mepc_before`
  was inside the spin loop (`0x8000034a` in the printed raw trials).
  No spurious traps on any run.
- The CLINT compare never fired early and never missed: the
  `t_entry >= cmp` check passed on all 2976 recorded trials, so every
  measured offset is non-negative (observed minima 105, 102, 115
  ticks), and every one of the 3446 attempts produced its trap.
- The delivery offset is the emulator's interrupt-notice plus
  trap-entry latency, measured in wall-clock ticks: the bulk sits at
  p50 152 to 198 ticks (15.2 to 19.8 us), the floor at 102 to 115
  ticks. The tail (p99 814 to 881, max 992 to 998) is host scheduling
  that stayed under the 1000-tick re-run bound; 85 to 252 attempts per
  run were re-run after crossing it.
- Spot check on the raw trials: run 1 raw[0] has
  `cmp=0x99357b`, `t_entry=0x993616`, offset 155 ticks, inside the
  printed distribution; run 2 raw[0] offset 260; run 3 raw[0] offset
  129.

## What this characterizes, and its limits

This experiment cannot separate the CLINT's compare (exact by the
`mtime >= mtimecmp` rule, confirmed by the never-negative offset on
2976 trials and zero missed traps) from QEMU's interrupt-delivery
path and the wall time the vcpu needs to execute between the compare
match and the trap-entry stamp. The measured spread is therefore the
emulator's end-to-end timer delivery latency on this host, not
silicon behavior; QEMU 8.2.2 timing has host noise and the tail is
dominated by it.

Two consequences for reading the numbers honestly. First, the
1000-tick re-run bound truncates the tail: the reported max is a lower
bound on the worst case under host load, and the retry counts (109,
252, 85) are part of the result, not an embarrassment to hide. Second,
the cycle-unit numbers are the tick measurements scaled by the
in-module 149 ratio, not independently stamped at the compare instant,
which no readable register provides.

## Raw QEMU run logs

The complete terminal output of all three runs, exactly as captured
(the program ends by parking in `wfi`; each run was taken under
`timeout 600`).
## Run 1 (bench-logs/run1.log)

```
mtimecmp: CLINT mtimecmp delivery accuracy (backlog item 36)
mhartid=0
mtvec=0x800001e4 mscratch=0x800033e0
cal: 10000000 mtime ticks -> rdcycle delta 1497838305 (ratio 149 rdcycle units per tick)
cal: 64 back-to-back (rdcycle, mtime) pairs monotonic: ok
trials: 1000, mtimecmp = mtime+5000 ticks, offset bound 1000 ticks
trap_count=1109 (attempts 1109, arm retries 109)
raw[0]: cmp=0x99357b t_pre=0x9922df c_pre=0x50fb8148bc0 t_entry=0x993616 c_entry=0x50fb81fc8c3 mepc_before=0x8000034a mcause=0x8000000000000007
raw[1]: cmp=0x994a9b t_pre=0x9937ba c_pre=0x50fb820bf08 t_entry=0x994b22 c_entry=0x50fb82c1867 mepc_before=0x8000034a mcause=0x8000000000000007
trials recorded: 1000 (first 8 discarded from stats)
in-trial rdcycle-vs-mtime slope: sum_c=764376195 sum_t=5103908 slope=149 (calibration ratio 149)
offset_ticks: min=105 p50=195 p99=881 max=996 mean=236
offset_cyc  : min=15645 p50=29055 p99=131269 max=148404 mean=35175
hist offset_ticks (n=992):
  105: 1
  112: 1
  113: 1
  115: 2
  116: 2
  117: 5
  118: 7
  119: 8
  120: 6
  121: 11
  122: 10
  123: 8
  124: 17
  125: 9
  126: 9
  127: 11
  128: 8
  129: 9
  130: 5
  131: 2
  132: 4
  133: 3
  134: 1
  135: 1
  136: 5
  137: 5
  138: 1
  139: 1
  141: 3
  142: 1
  143: 2
  144: 3
  145: 2
  146: 1
  147: 6
  148: 2
  149: 5
  150: 3
  151: 4
  152: 3
  153: 3
  154: 4
  155: 2
  156: 3
  157: 3
  158: 11
  159: 6
  160: 7
  161: 5
  162: 1
  163: 4
  164: 5
  165: 3
  166: 8
  167: 8
  168: 5
  169: 6
  170: 8
  171: 8
  172: 8
  173: 12
  174: 7
  175: 6
  176: 7
  177: 1
  178: 7
  179: 12
  180: 9
  181: 12
  182: 12
  183: 13
  184: 13
  185: 5
  186: 8
  187: 7
  188: 11
  189: 7
  190: 12
  191: 9
  192: 8
  193: 10
  194: 9
  195: 7
  196: 8
  197: 5
  198: 12
  199: 7
  200: 5
  201: 5
  202: 10
  203: 9
  204: 8
  205: 5
  206: 5
  207: 5
  208: 5
  209: 6
  210: 6
  211: 6
  212: 7
  213: 8
  214: 6
  215: 3
  216: 4
  217: 3
  218: 3
  219: 5
  220: 5
  221: 2
  222: 6
  223: 5
  224: 2
  225: 2
  226: 4
  227: 1
  228: 5
  229: 1
  232: 4
  233: 3
  234: 4
  235: 1
  236: 2
  238: 2
  239: 2
  240: 3
  241: 1
  242: 2
  243: 6
  244: 2
  245: 3
  246: 3
  247: 1
  248: 2
  250: 2
  251: 2
  252: 2
  253: 5
  254: 2
  255: 1
  256: 2
  257: 5
  258: 3
  259: 3
  260: 4
  261: 5
  262: 2
  263: 4
  264: 4
  265: 4
  266: 4
  267: 6
  268: 4
  269: 1
  270: 1
  271: 4
  272: 6
  273: 3
  275: 4
  276: 2
  277: 1
  278: 3
  279: 3
  280: 3
  281: 4
  282: 3
  283: 1
  284: 2
  285: 4
  286: 5
  287: 3
  288: 1
  292: 1
  295: 3
  296: 4
  298: 1
  299: 3
  301: 4
  302: 1
  303: 3
  304: 1
  305: 1
  307: 4
  308: 1
  309: 2
  310: 1
  312: 5
  313: 1
  314: 1
  316: 1
  317: 2
  319: 1
  321: 1
  322: 1
  324: 1
  325: 1
  326: 1
  329: 1
  330: 1
  331: 1
  338: 1
  340: 1
  342: 1
  343: 1
  345: 1
  346: 1
  348: 1
  355: 1
  359: 1
  360: 1
  361: 1
  362: 1
  363: 1
  376: 1
  377: 1
  406: 1
  409: 1
  413: 1
  419: 1
  421: 2
  423: 1
  427: 2
  430: 1
  432: 1
  436: 1
  437: 1
  453: 1
  455: 1
  458: 1
  462: 1
  464: 1
  467: 1
  470: 1
  479: 1
  486: 1
  487: 1
  488: 1
  492: 1
  493: 1
  494: 1
  500: 1
  503: 1
  504: 2
  507: 1
  513: 1
  514: 1
  515: 1
  516: 1
  520: 1
  522: 1
  543: 1
  549: 1
  556: 1
  561: 1
  566: 1
  567: 1
  571: 1
  582: 1
  583: 1
  589: 1
  590: 1
  597: 1
  600: 1
  606: 1
  608: 1
  613: 1
  614: 2
  616: 1
  617: 1
  647: 1
  653: 1
  660: 1
  671: 1
  672: 1
  673: 1
  683: 1
  691: 1
  695: 1
  698: 1
  706: 1
  751: 1
  757: 1
  759: 1
  771: 1
  794: 1
  795: 1
  811: 1
  827: 1
  866: 1
  876: 1
  880: 1
  881: 1
  885: 1
  900: 1
  902: 1
  937: 1
  945: 1
  956: 1
  978: 2
  996: 1
RESULT: PASS
done
```

## Run 2 (bench-logs/run2.log)

```
mtimecmp: CLINT mtimecmp delivery accuracy (backlog item 36)
mhartid=0
mtvec=0x800001e4 mscratch=0x800033e0
cal: 10000000 mtime ticks -> rdcycle delta 1498714590 (ratio 149 rdcycle units per tick)
cal: 64 back-to-back (rdcycle, mtime) pairs monotonic: ok
trials: 1000, mtimecmp = mtime+5000 ticks, offset bound 1000 ticks
trap_count=1252 (attempts 1252, arm retries 252)
raw[0]: cmp=0x9ab39e t_pre=0x9aa04c c_pre=0x51631867cb0 t_entry=0x9ab4a2 c_entry=0x516319262be mepc_before=0x8000034a mcause=0x8000000000000007
raw[1]: cmp=0x9ac9dd t_pre=0x9ab683 c_pre=0x51631937d48 t_entry=0x9acdb5 c_entry=0x51631a10ea9 mepc_before=0x8000034a mcause=0x8000000000000007
trials recorded: 1000 (first 8 discarded from stats)
in-trial rdcycle-vs-mtime slope: sum_c=769129125 sum_t=5135454 slope=149 (calibration ratio 149)
offset_ticks: min=102 p50=198 p99=822 max=998 mean=248
offset_cyc  : min=15198 p50=29502 p99=122478 max=148702 mean=37043
hist offset_ticks (n=992):
  102: 1
  113: 1
  115: 1
  116: 1
  119: 1
  121: 1
  125: 1
  126: 1
  129: 1
  130: 1
  131: 1
  132: 1
  133: 4
  134: 2
  135: 1
  136: 2
  137: 5
  138: 4
  139: 6
  140: 6
  141: 6
  142: 9
  143: 4
  144: 4
  145: 5
  146: 8
  147: 7
  148: 5
  149: 6
  150: 11
  151: 8
  152: 13
  153: 4
  154: 8
  155: 10
  156: 8
  157: 6
  158: 11
  159: 8
  160: 3
  161: 7
  162: 10
  163: 15
  164: 13
  165: 2
  166: 11
  167: 9
  168: 16
  169: 6
  170: 11
  171: 7
  172: 8
  173: 15
  174: 8
  175: 9
  176: 11
  177: 8
  178: 11
  179: 8
  180: 7
  181: 8
  182: 7
  183: 8
  184: 8
  185: 10
  186: 10
  187: 9
  188: 5
  189: 10
  190: 4
  191: 7
  192: 7
  193: 4
  194: 6
  195: 1
  196: 6
  197: 6
  198: 11
  199: 4
  200: 3
  201: 7
  202: 4
  203: 5
  204: 4
  205: 3
  207: 5
  208: 4
  209: 4
  210: 8
  211: 4
  212: 4
  213: 5
  214: 2
  215: 4
  216: 1
  217: 6
  218: 4
  219: 2
  220: 9
  221: 2
  222: 4
  223: 3
  224: 4
  225: 4
  226: 4
  227: 3
  228: 4
  229: 1
  230: 2
  231: 4
  232: 2
  233: 5
  234: 2
  235: 3
  236: 5
  237: 3
  238: 2
  240: 5
  241: 3
  242: 1
  243: 2
  244: 1
  245: 5
  246: 4
  247: 4
  248: 3
  249: 7
  250: 6
  251: 7
  252: 4
  253: 3
  254: 3
  255: 1
  256: 2
  257: 3
  258: 2
  259: 1
  260: 4
  261: 5
  262: 1
  263: 1
  264: 1
  265: 3
  266: 5
  267: 4
  268: 6
  269: 3
  270: 2
  271: 1
  272: 5
  273: 2
  274: 2
  275: 4
  276: 5
  277: 2
  278: 3
  279: 5
  280: 3
  281: 1
  282: 1
  283: 1
  284: 3
  285: 3
  286: 1
  288: 1
  290: 2
  291: 2
  292: 2
  293: 1
  294: 5
  295: 1
  296: 1
  297: 1
  298: 2
  299: 1
  300: 3
  301: 2
  302: 1
  303: 1
  304: 3
  306: 1
  307: 1
  309: 1
  310: 2
  311: 1
  313: 2
  316: 2
  317: 2
  319: 1
  320: 1
  321: 1
  325: 2
  328: 1
  329: 1
  331: 1
  333: 1
  334: 1
  335: 1
  336: 1
  338: 2
  339: 2
  340: 1
  346: 1
  347: 2
  354: 1
  356: 1
  357: 1
  360: 1
  362: 2
  367: 1
  368: 1
  377: 1
  379: 1
  380: 2
  382: 2
  383: 1
  384: 1
  386: 1
  390: 1
  392: 1
  399: 1
  401: 1
  404: 1
  409: 1
  411: 1
  421: 1
  423: 2
  424: 1
  429: 2
  430: 2
  438: 1
  439: 1
  440: 1
  442: 2
  445: 1
  446: 1
  449: 1
  451: 1
  454: 2
  456: 1
  457: 1
  465: 1
  472: 1
  476: 1
  483: 1
  485: 1
  486: 1
  492: 1
  493: 1
  498: 1
  502: 1
  507: 1
  508: 1
  509: 3
  513: 2
  516: 1
  521: 1
  527: 1
  528: 2
  530: 1
  538: 1
  541: 1
  543: 1
  547: 1
  551: 1
  552: 1
  556: 1
  563: 1
  564: 1
  566: 1
  570: 1
  572: 1
  574: 1
  576: 1
  579: 3
  582: 1
  585: 1
  594: 1
  597: 1
  601: 1
  605: 2
  607: 1
  614: 2
  615: 1
  616: 1
  625: 1
  642: 1
  645: 1
  648: 1
  649: 1
  662: 1
  667: 1
  670: 1
  672: 2
  674: 1
  687: 2
  690: 1
  692: 1
  693: 1
  700: 1
  713: 1
  714: 1
  723: 1
  732: 1
  735: 2
  744: 1
  767: 1
  792: 1
  822: 1
  848: 1
  855: 1
  861: 1
  876: 1
  897: 1
  898: 1
  904: 1
  931: 1
  998: 1
RESULT: PASS
done
```

## Run 3 (bench-logs/run3.log)

```
mtimecmp: CLINT mtimecmp delivery accuracy (backlog item 36)
mhartid=0
mtvec=0x800001e4 mscratch=0x800033e0
cal: 10000000 mtime ticks -> rdcycle delta 1497828885 (ratio 149 rdcycle units per tick)
cal: 64 back-to-back (rdcycle, mtime) pairs monotonic: ok
trials: 1000, mtimecmp = mtime+5000 ticks, offset bound 1000 ticks
trap_count=1085 (attempts 1085, arm retries 85)
raw[0]: cmp=0x995d83 t_pre=0x994ab2 c_pre=0x517e901d91f t_entry=0x995e04 c_entry=0x517e90d257c mepc_before=0x8000034a mcause=0x8000000000000007
raw[1]: cmp=0x99729e t_pre=0x99601e c_pre=0x517e90e616c t_entry=0x997329 c_entry=0x517e919846b mepc_before=0x8000034a mcause=0x8000000000000007
trials recorded: 1000 (first 8 discarded from stats)
in-trial rdcycle-vs-mtime slope: sum_c=753793560 sum_t=5032872 slope=149 (calibration ratio 149)
offset_ticks: min=115 p50=152 p99=814 max=992 mean=203
offset_cyc  : min=17135 p50=22648 p99=121286 max=147808 mean=30378
hist offset_ticks (n=992):
  115: 1
  116: 5
  117: 1
  118: 3
  119: 9
  120: 9
  121: 5
  122: 14
  123: 16
  124: 21
  125: 22
  126: 31
  127: 37
  128: 28
  129: 36
  130: 34
  131: 21
  132: 25
  133: 20
  134: 20
  135: 18
  136: 18
  137: 8
  138: 4
  139: 8
  140: 5
  141: 7
  142: 7
  143: 3
  144: 8
  145: 8
  146: 12
  147: 3
  148: 8
  149: 6
  150: 7
  151: 2
  152: 8
  153: 4
  154: 5
  155: 9
  156: 4
  157: 5
  158: 5
  159: 11
  160: 7
  161: 4
  162: 12
  163: 5
  164: 6
  165: 5
  166: 7
  167: 6
  168: 5
  169: 7
  170: 10
  171: 9
  172: 2
  173: 9
  174: 4
  175: 9
  176: 13
  177: 9
  178: 7
  179: 2
  180: 6
  181: 6
  182: 3
  183: 5
  184: 3
  185: 3
  186: 7
  187: 1
  188: 6
  189: 3
  190: 4
  191: 2
  192: 4
  193: 3
  194: 8
  195: 2
  196: 5
  198: 2
  199: 5
  200: 1
  201: 3
  202: 2
  203: 6
  204: 3
  205: 2
  206: 3
  208: 1
  211: 1
  212: 1
  213: 2
  214: 2
  215: 4
  216: 2
  221: 1
  222: 2
  224: 2
  226: 1
  227: 2
  228: 1
  229: 3
  230: 4
  231: 1
  232: 1
  233: 2
  234: 1
  236: 1
  237: 1
  238: 3
  239: 4
  242: 1
  244: 2
  245: 1
  246: 2
  248: 2
  249: 1
  250: 1
  252: 1
  253: 2
  254: 2
  255: 1
  256: 3
  257: 4
  258: 1
  261: 2
  263: 1
  264: 1
  265: 3
  267: 2
  268: 1
  269: 3
  270: 1
  271: 1
  272: 2
  274: 2
  275: 1
  276: 1
  277: 2
  279: 1
  284: 1
  285: 1
  288: 2
  289: 1
  290: 1
  292: 1
  295: 1
  300: 1
  308: 1
  309: 1
  315: 1
  322: 2
  323: 1
  325: 1
  326: 1
  331: 1
  334: 1
  335: 1
  337: 1
  339: 1
  340: 3
  341: 1
  342: 1
  347: 1
  350: 1
  354: 1
  357: 1
  358: 1
  361: 1
  366: 1
  367: 1
  370: 1
  371: 1
  373: 1
  379: 1
  381: 1
  388: 1
  393: 1
  399: 1
  402: 1
  412: 1
  423: 1
  428: 1
  435: 3
  449: 1
  457: 1
  458: 1
  461: 1
  468: 1
  485: 1
  492: 1
  494: 1
  496: 1
  502: 1
  504: 2
  517: 1
  519: 1
  520: 1
  523: 1
  526: 1
  529: 1
  549: 1
  553: 1
  556: 1
  557: 1
  562: 1
  565: 1
  567: 2
  572: 1
  573: 1
  577: 1
  584: 1
  587: 1
  589: 1
  596: 1
  618: 1
  625: 1
  629: 1
  631: 1
  632: 1
  641: 1
  649: 1
  652: 1
  663: 1
  667: 1
  672: 1
  680: 1
  682: 1
  683: 1
  699: 1
  703: 1
  707: 1
  709: 1
  712: 1
  725: 1
  726: 1
  736: 1
  740: 1
  744: 1
  748: 1
  754: 1
  780: 1
  787: 1
  788: 1
  799: 1
  807: 1
  814: 1
  845: 1
  865: 1
  867: 1
  873: 1
  890: 1
  902: 1
  909: 1
  988: 1
  992: 1
RESULT: PASS
done
```

