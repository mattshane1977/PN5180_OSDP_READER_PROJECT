---
name: ESP-IDF build environment — PATH must be set per command
description: ESP-IDF env vars don't persist between Bash tool calls; must prepend full PATH every time
type: feedback
originSessionId: a3e8548a-cc89-4b3c-a902-a1e183651511
---
ESP-IDF environment does not persist between Bash tool calls. Every `idf.py` invocation must be prefixed with:

```
export PATH="/home/matt/.espressif/tools/xtensa-esp32s3-elf/esp-12.2.0_20230208/xtensa-esp32s3-elf/bin:/home/matt/.espressif/python_env/idf5.1_py3.13_env/bin:/home/matt/esp/esp-idf/tools:$PATH" && export IDF_PATH=/home/matt/esp/esp-idf
```

**Why:** Shell state resets between tool calls; `source export.sh` in a prior call has no effect on the next.
**How to apply:** Always prepend this PATH export before any `idf.py build`, `idf.py set-target`, etc.
