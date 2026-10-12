# BF-Tree v2 Correctness Fuzzing Harnesses

This directory contains libFuzzer harnesses for the BF-Tree WAL decoder, recovery, and end-to-end SQL execution.

## Harnesses

1. **fuzz_bfwal_decoder**: Tests the WAL record-batch decoder and index
   - `sqlite3BfWalIterInit` / `sqlite3BfWalIterNext`
   - `sqlite3BfWalIndexAddFrame` / `sqlite3BfWalIndexGet`
   - `sqlite3BfWalIndexForEachPage` / `sqlite3BfWalIndexPrune`

2. **fuzz_bfwal_recovery**: Tests end-to-end recovery with mutated WAL files
   - Builds a database with buffered operations
   - Mutates the WAL using fuzzer input
   - Attempts recovery and verifies integrity

3. **fuzz_sql**: Tests end-to-end SQL semantics with BF enabled
   - Random SQL statements (INSERT/UPDATE/DELETE)
   - Transactions (COMMIT/ROLLBACK/SAVEPOINT)
   - Checkpoints and pragmas
   - Multi-connection reader/writer scenarios

## Build Commands

All builds use clang 22 with ASan+UBSan and debug assertions enabled.

### Decoder Harness (smallest, fastest, isolated)
```bash
clang -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
      -DSQLITE_DEBUG -DSQLITE_BF_INSERT_BUFFERING \
      -I ../../src test/fuzz/fuzz_bfwal_decoder.c -o test/fuzz/fuzz_bfwal_decoder
```

### Recovery Harness (end-to-end with WAL mutation)
```bash
clang -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
      -DSQLITE_DEBUG -DSQLITE_BF_INSERT_BUFFERING -DSQLITE_ENABLE_FTS4 -DSQLITE_ENABLE_RTREE \
      -I build -I src test/fuzz/fuzz_bfwal_recovery.c build/sqlite3.c \
      -o test/fuzz/fuzz_bfwal_recovery -lm -lz
```

### SQL Harness (end-to-end SQL with random ops)
```bash
clang++ -g -O1 -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all \
        -DSQLITE_DEBUG -DSQLITE_BF_INSERT_BUFFERING -DSQLITE_ENABLE_FTS4 -DSQLITE_ENABLE_RTREE \
        -I build -I src test/fuzz/fuzz_sql.c build/sqlite3.c \
        -o test/fuzz/fuzz_sql -lm -lz
```

## Running Fuzzing Sessions

### Single-threaded with bounded run count:
```bash
mkdir -p corpus/bfwal corpus/recovery corpus/sql

./fuzz_bfwal_decoder corpus/bfwal -runs=1000000 -max_total_time=600 -max_len=4096
./fuzz_bfwal_recovery corpus/recovery -runs=500000 -max_total_time=600 -max_len=8192
./fuzz_sql corpus/sql -runs=500000 -max_total_time=600 -max_len=4096
```

### Parallel fuzzing (if resources allow):
```bash
./fuzz_bfwal_decoder corpus/bfwal -jobs=4 -workers=4 -max_len=4096
```

### Triage a crash:
```bash
./fuzz_bfwal_decoder crash-<hash>  # Reproduce the crash
./fuzz_bfwal_decoder -minimize_crash=1 -runs=100000 crash-<hash>  # Minimize
```

## Expected Results

- No crashes on arbitrary input
- No ASAN/UBSan violations
- No assertion failures
- `BFWAL_CORRUPT` errors are acceptable for malformed payloads
- Recovery must succeed on valid-header payloads even if content is corrupted
- Integrity checks must pass or report SQLITE_CORRUPT (not crash)

## Seed Corpus

For faster fuzzing, seed the corpus with real WAL frames from existing databases:
```bash
# Extract WAL frames from a real database
python3 -c "
import struct
with open('some.db-wal', 'rb') as f:
    header = f.read(32)
    while True:
        frame_hdr = f.read(24)
        if not frame_hdr: break
        f.read(4096)  # Skip page payload
        with open(f'corpus/bfwal/frame_{hash(frame_hdr):016x}', 'wb') as out:
            out.write(header + frame_hdr)
"
```
