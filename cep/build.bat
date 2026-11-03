@echo off
rem Builds cbm.exe with the flags used for every reported result.
g++ -std=c++17 -O2 -Wall -Wextra -o cbm.exe src\main.cpp src\loader.cpp src\selector.cpp src\agent.cpp src\dataset.cpp src\experiments.cpp src\tests.cpp src\menu.cpp
