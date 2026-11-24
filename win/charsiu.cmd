@echo off
rem charsiu on Windows: the front door is charsiu.ps1 one folder up. Run
rem through -File with the policy bypassed for this process only, so the
rem command works whatever the machine's execution policy is.
rem SPDX-License-Identifier: GPL-2.0-only
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\charsiu.ps1" %*
exit /b %ERRORLEVEL%
