// main/g2048_ui.h —— 2048 应用自己的界面层(竖屏 240x320,与基线 demo 测试菜单无关)。
//
// 界面完全重新设计:顶部标题栏(分数/电量)、中部 4x4 棋盘、底部方向轮盘与操作提示,
// 结束时在棋盘上盖一层结算浮层。所有函数内部自行持有 LVGL 锁,调用方(输入任务、
// app_main)不必关心加锁,也就不存在"忘加锁"的漏网之鱼。
#pragma once

#include <stdbool.h>

#include "g2048_model.h"

// 创建主界面并载入屏幕。必须在 bsp_lvgl_init() 成功之后调用。
bool g2048_ui_init(void);

// 删除界面、停止定时器并释放对象。可在没有持锁的情况下调用;重复调用安全。
void g2048_ui_deinit(void);

// 用当前模型状态刷新棋盘、分数、步数与结算浮层。
void g2048_ui_render(const g2048_t *game);

// 更新方向轮盘的高亮(只改样式,不重画棋盘)。
void g2048_ui_set_direction(g2048_dir_t dir);

// 短暂显示"恭喜达成 2048！",约 2 秒后自行消失。
// 界面文案全部收在本文件对应的 .c 里,字体覆盖门禁只需扫那一个文件。
void g2048_ui_show_win_banner(void);

// 立即隐藏提示条。
void g2048_ui_hide_banner(void);

// 显示"即将休眠"提示。进入 deep sleep 前调用。
void g2048_ui_show_sleep_notice(void);

// 刷新右上角电量读数;读不到时显示 "--" 而不是画一个假数字。
void g2048_ui_refresh_battery(void);
