# Embedded Device Rules: Harmony Hub & Elite Remote

## MANDATORY RULES FOR ALL HUB & ELITE OPERATIONS

1. **KILL -9 ONLY & ALWAYS CLEAN UP PROCESSES**:
   - `kill` without `-9` FAILS on these embedded pthreads devices.
   - ALWAYS use `killall -9 <proc_name> 2>/dev/null` or `kill -9 <pid>`.
   - Before launching ANY binary or script that touches `/dev/rfspi`, `/dev/input/*`, I2C, or serial, YOU MUST KILL OLD / DUPLICATE PROCESSES FIRST.

2. **SINGLE PROCESS ON `/dev/rfspi`**:
   - The Atheros AR9344 SoC on the Hub shares SPI between CC2544 and NOR flash.
   - Concurrent access corrupts the SPI arbiter and freezes the CPU.
   - NEVER run two processes opening `/dev/rfspi` simultaneously.
   - Always verify and kill background instances before starting a test process.

3. **MINIMAL BUSYBOX ENVIRONMENT**:
   - Neither Hub nor Elite has `head`, `tail`, or `grep -E`.
   - Never run `head`, `tail`, or `grep -E` in SSH commands.
   - Use standard `cat`, `sed`, `grep`, or `awk`.
