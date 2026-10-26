#!/bin/sh
# Lowers this shell's priority class so a parallel build leaves the desktop
# usable.  Children inherit the class from the shell, so make, g++ and the
# linker all run below normal without wrapping each command.  Source it:
#
#   . tools/lowpri.sh
#
# MSYS's $$ is not a Windows PID; /proc/$$/winpid is (busybox-w32's $$ already
# is one).  Passing an MSYS $$ to Get-Process would touch an unrelated process
# and look like it worked.  `read` is a builtin, because the PATH may already
# hold a cat that cannot see MSYS's /proc.  PRIORITY=Idle for the lowest
# class, PRIORITY=Normal to skip.
PRIORITY=${PRIORITY:-BelowNormal}
if [ "$PRIORITY" != "Normal" ]; then
    wpid=$$
    if [ -r /proc/$$/winpid ]; then
        read -r wpid < /proc/$$/winpid
    fi
    powershell -NoProfile -Command "(Get-Process -Id $wpid).PriorityClass='$PRIORITY'" >/dev/null 2>&1 || true
fi
