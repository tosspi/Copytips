/* 版本号唯一来源：copytips.cpp 与 copytips.rc 都从这里读，避免两处不一致。
 *
 * 命名规则（SemVer 2.0.0）：主版本.次版本.修订号
 *   主版本  用户可感知的行为或形态大改（旧观感/用法不再成立）      例：方窗 -> 圆角胶囊
 *   次版本  视觉风格或新能力调整，用法完全兼容                    例：更小 + 半透明
 *   修订号  只修 bug / 只调参数，不改外观也不改行为               例：修描边亮度
 * 每次改动必须至少递增其中一段，禁止改了代码却不改版本号。
 */
#ifndef COPYTIPS_VERSION_H
#define COPYTIPS_VERSION_H

#define APP_VERSION_MAJOR 2
#define APP_VERSION_MINOR 3
#define APP_VERSION_PATCH 0

#define APP_VERSION_STR   "2.3.0"
#define APP_VERSION_WSTR  L"2.3.0"

#endif
