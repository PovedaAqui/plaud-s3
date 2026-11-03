# Security

## Reporting

Use GitHub's private vulnerability reporting (Security → Advisories) rather than a public
issue.

## Scope

The ingest server has **no authentication**: `/ws/audio` accepts any client that can reach
it. Run it on a trusted LAN, or put it behind a reverse proxy that authenticates. Do not
expose port 8000 directly to the internet.

Credentials for Wi-Fi and the server address live in `scripts/wifi.env` and
`firmware/sdkconfig.local`; both are ignored by git. They are compiled into the firmware
image, so treat a flashed device as containing them.
