// Стан будинку, спільний для всіх модулів.
#include "zh.h"

SecData sec = {SEC_DIS, SM_NONE, SN_NONE, 0, 0, 0, 0};
EnvData env = {0, 0, false, 0, false, 0, 0, true, 0, false, false, 0, 0};
IoData io = {false, false, false, false, 0, {0, 0, 0}};
DevData dev = {OM_AUTO, false, WM_AUTO, 60, 100, false, OM_AUTO, false, 0xFFC98AUL, 70};
PrkData prk = {GM_AUTO, GS_CLOSED, -1, 0, 0, 'r', "StOP"};
SysData sys = {};
Ovr ovr = {};
