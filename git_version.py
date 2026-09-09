# git_version.py
from SCons.Script import Import
import subprocess

Import("env")

try:
    git_version = subprocess.check_output(
        ["git", "rev-parse", "--short", "HEAD"],
        text=True,
        stderr=subprocess.DEVNULL,
    ).strip()
except (FileNotFoundError, subprocess.CalledProcessError):
    git_version = "snapshot"

env.Append(
    CPPDEFINES=[
        ("GIT_VERSION", '\\"{}\\"'.format(git_version))
    ]
)

