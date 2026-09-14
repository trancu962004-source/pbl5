@echo off
title Patient CAN Dashboard

cd /d D:\nckh_test1\python_can

echo Dang kiem tra va cai thu vien...
python -m pip install python-can fastapi uvicorn websockets wsproto

echo.
echo Dang mo web dashboard...
start http://127.0.0.1:8000

echo.
echo Dang chay Python CAN reader...
python patient_can_web_dashboard_old.py --channel COM27 --bitrate 125000 --web

echo.
echo Chuong trinh da dung hoac co loi.
pause