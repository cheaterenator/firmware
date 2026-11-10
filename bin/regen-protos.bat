@ECHO OFF
SETLOCAL

REM regen-protos.sh also adds Telemetry.local_stats_extended and generates protobufs-private/, so
REM run that rather than a copy of it here. Needs Git for Windows' bash (not WSL's).
SET "GIT_BASH=%ProgramFiles%\Git\bin\bash.exe"
IF NOT EXIST "%GIT_BASH%" (
    ECHO Git Bash not found at "%GIT_BASH%" - run bin/regen-protos.sh from a bash shell instead.
    EXIT /B 1
)
"%GIT_BASH%" bin/regen-protos.sh

ENDLOCAL
EXIT /B %ERRORLEVEL%
