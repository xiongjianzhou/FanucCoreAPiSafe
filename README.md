# FOCAS2 Web API Server (Linux)

Pure C HTTP server linked against FANUC FOCAS2 library (`libfwlib32.so`).

**Supported platform**: Linux x86 (32-bit FOCAS library)

## Quick Start

### Build

```bash
./build.sh
# Requires: g++-multilib, libc6:i386, libstdc++6:i386
```

Or manually:

```bash
g++ -m32 -o focaswebapi focaswebapi.c \
    -I./NativeLib/Linux \
    -L./NativeLib/Linux \
    -lfwlib32 -lstdc++ -lpthread
```

### Run

```bash
LD_LIBRARY_PATH=./NativeLib/Linux ./focaswebapi [port]
```

Default port: 5000

### Verify

```bash
curl http://localhost:5000/api/focas/ping
# {"Success":true,"Data":{"status":"ok","time":"14:30:00"},"ErrorCode":0}

curl "http://localhost:5000/api/focas/sysinfo?ip=192.168.0.47"
# {"Success":true,"Data":{"Series":"D4G3","Version":"30.0","AxisCount":3},"ErrorCode":0}
```

## API Endpoints

All endpoints support `?ip=&port=8193&timeout=3` query parameters.

> ⚠️ **v0.2 变更：仅保留程序上传接口（PUT/POST），其他所有写入操作均已禁用，返回 403。**

### Monitor

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/focas/ping` | Health check |
| GET | `/api/focas/sysinfo` | CNC system info |
| GET | `/api/focas/status` | Run status |
| GET | `/api/focas/monitor` | Full monitor data |
| GET | `/api/focas/alarm` | Alarm status |
| GET | `/api/focas/override` | Feed/spindle override |
| ~~POST~~ | ~~`/api/focas/start`~~ | ~~Cycle start (disabled)~~ |
| ~~POST~~ | ~~`/api/focas/reset`~~ | ~~Reset/stop (disabled)~~ |
| ~~POST~~ | ~~`/api/focas/alarm/clear`~~ | ~~Clear alarm (disabled)~~ |

### Programs

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/focas/programs` | List programs |
| GET | `/api/focas/programs/{num}` | Read program content |
| **PUT** | **`/api/focas/programs`** | **Upload program (JSON)** |
| ~~DELETE~~ | ~~`/api/focas/programs/{num}`~~ | ~~Delete program (disabled)~~ |
| GET | `/api/focas/programs/actpt` | Active program pointer |
| ~~POST~~ | ~~`/api/focas/programs/{num}/run`~~ | ~~Run program (disabled)~~ |
| ~~POST~~ | ~~`/api/focas/programs/{num}/autostart`~~ | ~~Safe autostart (disabled)~~ |

### Macros

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/focas/macros?start=&count=` | Range read macros |
| GET | `/api/focas/macros/user/{num}` | Read user macro |
| ~~PUT~~ | ~~`/api/focas/macros/user`~~ | ~~Write user macro (disabled)~~ |
| GET | `/api/focas/macros/pcode/{num}` | Read P-code macro |
| ~~PUT~~ | ~~`/api/focas/macros/pcode`~~ | ~~Write P-code macro (disabled)~~ |
| POST | `/api/focas/macros/batch` | Batch read macros |

### PLC/PMC

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/focas/plc/{type}/{addr}` | Read PLC (numeric type) |
| GET | `/api/focas/plc?type=G&start=&count=` | Read PLC (letter type) |
| ~~PUT~~ | ~~`/api/focas/plc`~~ | ~~Write PLC (disabled)~~ |
| POST | `/api/focas/plc/batch` | Batch read PLC |

**Address type codes (FANUC standard):**

| Code | Letter | Description |
|------|--------|-------------|
| 0 | G | PMC→CNC signal |
| 1 | F | CNC→PMC signal |
| 2 | Y | PMC→machine output |
| 3 | X | Machine→PMC input |
| 4 | A | Message display |
| 5 | R | Internal relay |
| 6 | T | Timer |
| 7 | K | Keep relay |
| 8 | C | Counter |
| 9 | D | Data table |
| 10 | E | Extended |

### Parameters

| Method | Path | Description |
|--------|------|-------------|
| GET | `/api/focas/parameters/{num}` | Read parameter |
| GET | `/api/focas/parameters/axis/{num}` | Read axis-specific parameter |
| ~~PUT~~ | ~~`/api/focas/parameters`~~ | ~~Write parameter (disabled)~~ |

## Web UI

| Path | Description |
|------|-------------|
| `/` | Swagger UI |
| `/swagger` | Swagger UI |
| `/swagger.json` | Swagger spec |
| `/openapi.json` | OpenAPI spec |
| `/docs` | Human-readable docs |

## Response Format

```json
{
  "Success": true,
  "Data": { ... },
  "ErrorCode": 0
}
```

## Error Codes

| Code | Meaning |
|------|---------|
| 0 | OK (EW_OK) |
| -1 | CNC busy (EW_BUSY) |
| -16 | Socket error (EW_SOCKET) |
| 1 | Function not supported |
| 2 | Data length error |
| 5 | Data error |
| 6 | Function not available |
| 12 | Mode error (MEM/MDI required) |
| 403 | Write operation disabled (v0.2+) |

## Project Structure

```
FanucCoreApi/
├── focaswebapi.c          # C HTTP server source
├── build.sh               # Build script
├── NativeLib/Linux/       # FOCAS2 32-bit library
│   ├── fwlib32.h
│   └── libfwlib32.so*
├── index.html             # Swagger UI page
├── swagger.json           # Swagger spec
├── openapi.json           # OpenAPI spec
├── docs.html              # API docs
├── .gitignore
└── README.md
```
