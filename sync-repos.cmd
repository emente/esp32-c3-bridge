@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem =============================================================================
rem sync-repos.cmd -- commit and push every actively-used git repo across
rem this project to its GitHub origin.
rem
rem These are separate, independently-cloned working copies -- NOT the
rem esp32-c3-bridge repo's own citsviewer/esp32-c5-sniffer submodule
rem checkouts (nested under this script's own directory), which nothing
rem here touches since they aren't where changes actually get made:
rem   mqtt-bridge      -- %~dp0mqtt-bridge (a real submodule of this repo)
rem   esp32-c3-bridge  -- %~dp0 (this repo itself)
rem   esp32-c5-sniffer -- C:\tmp\esp32-c5-sniffer
rem   citsviewer       -- R:\sys\www\d\citsviewer
rem If any of these move, update the *_DIR values below.
rem
rem mqtt-bridge is synced BEFORE the top-level repo: if it gets a new commit
rem here, the top-level repo will see that submodule's checked-out commit as
rem "modified" and its own `git add -A` will pick up that pointer bump
rem automatically -- so it needs to run after, not before. esp32-c5-sniffer
rem and citsviewer are independent clones with no such relationship, so
rem their order relative to the others doesn't matter.
rem
rem Usage:
rem   sync-repos.cmd ["commit message"]
rem
rem With no message, a timestamped "chore: sync <UTC time>" is used. A repo
rem with nothing to commit is still checked for unpushed local commits and
rem pushed if any exist; a repo with neither is left untouched.
rem =============================================================================

if /I "%~1"=="/?" goto :usage
if /I "%~1"=="-h" goto :usage
if /I "%~1"=="--help" goto :usage

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"

set "MQTT_BRIDGE_DIR=%ROOT%\mqtt-bridge"
set "BRIDGE_DIR=%ROOT%"
set "SNIFFER_DIR=C:\tmp\esp32-c5-sniffer"
set "CITSVIEWER_DIR=R:\sys\www\d\citsviewer"

set "MESSAGE=%~1"
if not defined MESSAGE (
    for /f "delims=" %%T in ('powershell -NoProfile -Command "[DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ')" 2^>nul') do set "MESSAGE=chore: sync %%T"
)
if not defined MESSAGE set "MESSAGE=chore: sync"

echo Commit message: %MESSAGE%

set "FAILED=0"

call :sync_repo "%MQTT_BRIDGE_DIR%" "mqtt-bridge"
call :sync_repo "%SNIFFER_DIR%" "esp32-c5-sniffer"
call :sync_repo "%CITSVIEWER_DIR%" "citsviewer"
call :sync_repo "%BRIDGE_DIR%" "esp32-c3-bridge (top level)"

echo.
if "%FAILED%"=="0" (
    echo All repos synced successfully.
) else (
    echo Done, but %FAILED% repo^(s^) had errors -- see above.
)
exit /b %FAILED%

:usage
echo Usage: %~nx0 ["commit message"]
echo.
echo Commits any pending changes (git add -A + commit) and pushes every git
echo repo in this project -- mqtt-bridge, esp32-c5-sniffer, citsviewer, and
echo the top-level esp32-c3-bridge repo itself -- to its GitHub origin.
echo With no message, a timestamped default is used.
exit /b 0

rem -----------------------------------------------------------------------
rem :sync_repo <dir> <label>
rem -----------------------------------------------------------------------
:sync_repo
set "REPO_DIR=%~1"
set "REPO_LABEL=%~2"
echo.
echo ==== %REPO_LABEL% ====

if not exist "%REPO_DIR%\.git" (
    echo   no .git here, skipping
    goto :eof
)

git -C "%REPO_DIR%" rev-parse --is-inside-work-tree >nul 2>&1
if errorlevel 1 (
    echo   ERROR: not a usable git repo, skipping
    set /a FAILED+=1
    goto :eof
)

set "DIRTY=0"
for /f %%X in ('git -C "%REPO_DIR%" status --porcelain 2^>nul ^| find /c /v ""') do set "DIRTY=%%X"

if not "%DIRTY%"=="0" (
    echo   %DIRTY% changed file^(s^), committing...
    git -C "%REPO_DIR%" add -A
    if errorlevel 1 (
        echo   ERROR: git add failed, skipping
        set /a FAILED+=1
        goto :eof
    )
    git -C "%REPO_DIR%" commit -m "%MESSAGE%"
    if errorlevel 1 (
        echo   ERROR: commit failed ^(hook rejected it?^), skipping push
        set /a FAILED+=1
        goto :eof
    )
) else (
    echo   nothing to commit
)

set "BRANCH="
for /f "delims=" %%B in ('git -C "%REPO_DIR%" symbolic-ref --short -q HEAD 2^>nul') do set "BRANCH=%%B"
if not defined BRANCH (
    echo   ERROR: detached HEAD, not pushing
    set /a FAILED+=1
    goto :eof
)

git -C "%REPO_DIR%" rev-parse --abbrev-ref --symbolic-full-name "@{u}" >nul 2>&1
if errorlevel 1 (
    echo   no upstream tracking branch yet, pushing and setting one...
    git -C "%REPO_DIR%" push -u origin "%BRANCH%"
    if errorlevel 1 (
        echo   ERROR: push failed
        set /a FAILED+=1
    )
    goto :eof
)

set "AHEAD=0"
for /f %%X in ('git -C "%REPO_DIR%" log "@{u}..HEAD" --oneline 2^>nul ^| find /c /v ""') do set "AHEAD=%%X"
if "%AHEAD%"=="0" (
    echo   up to date with remote
    goto :eof
)

echo   %AHEAD% commit^(s^) to push...
git -C "%REPO_DIR%" push
if errorlevel 1 (
    echo   ERROR: push failed
    set /a FAILED+=1
)
goto :eof
