from SCons.Script import Import
import re

Import("env")

if env.get("PIOENV") == "supermini_ota":
    upload_port = env.get("UPLOAD_PORT", "")
    if re.fullmatch(r"(?i)COM\d+", str(upload_port)):
        raise RuntimeError(
            "supermini_ota requires an OTA hostname or IP address, "
            f"not serial port {upload_port}. Use -e supermini for USB upload."
        )