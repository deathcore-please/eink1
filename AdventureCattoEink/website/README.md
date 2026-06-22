# ANVI Device Manager

This is a static Web Serial page for adding and deleting books on the device over USB. It uploads `.txt` directly and converts `.epub` or text-based `.pdf` files to `.txt` in the browser before uploading.

Use Chrome or Edge, connect the ESP32-S3 over USB, open the page from HTTPS or localhost, then press **Connect**. The page talks to the firmware at `115200` baud with the `ACAT` serial protocol.

For local testing later, any static server works from this folder, for example:

```powershell
python -m http.server 8080
```

Then open `http://localhost:8080`.
