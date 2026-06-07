# GVT Cloud Server Control Plane

This folder contains the service-side management API for the GVT-g cloud
desktop MVP. It is intentionally self-contained and uses only the Python
standard library, so it can run directly on the openEuler host.

## Run

From the repository root:

```bash
export GVT_SERVER_API_PASSWORD='change-me'
export PYTHONPATH="$PWD/server-control"
python3 -m gvt_cloud_server --config /etc/gvt-cloud-server/config.json
```

Use `config.example.json` as the starting point. Keep real passwords and VM
images outside Git.

## API

- `POST /api/login`
- `GET /api/status`
- `GET /api/desktops`
- `GET /api/desktops/{id}`
- `POST /api/desktops/{id}/start`
- `POST /api/desktops/{id}/stop`
- `POST /api/desktops/{id}/restart`
- `POST /api/desktops/{id}/mode`
- `POST /api/desktops/{id}/profile`
- `POST /api/desktops/{id}/resources`
- `GET /api/gvtg-profiles`
- `POST /api/output/select`
- `POST /api/input/select`
- `POST /api/audio/select`
- `GET /api/logs/{id}`

Authenticated requests use `Authorization: Bearer <token>` or the login cookie.

## Tests

```bash
python -m unittest discover -s server-control/tests
python -m compileall -q server-control/gvt_cloud_server
```
