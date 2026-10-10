# 无头开发板上的真实候选窗 E2E

入口为 `fcitx5-wetype/e2e/run_visual_e2e.sh`。它创建独立的 Xvfb 显示、D-Bus 会话、配置和词库目录，启动真实 GTK3 输入框，使用 GTK 的 Fcitx 输入模块和 Fcitx5 classicui 候选窗。键盘输入由 xdotool/XTest 注入，没有直接调用输入法的 ProcessKeyEvent，也没有自行绘制候选窗。

## 依赖

- Xvfb、xdotool、xwininfo（Ubuntu 的 `x11-utils`）。
- Python3、D-Bus bindings、PyGObject、GTK3 introspection（`python3-dbus python3-gi gir1.2-gtk-3.0`）。
- GTK3 的 Fcitx5 输入模块（`fcitx5-frontend-gtk3`）。
- 中文字体，例如 `fonts-wqy-zenhei`。
- Fcitx5、fcitx5-remote，以及同版本的 `xcb`、`classicui` 插件和默认主题。
- 为目标主机编译的 WeType 插件，以及已经可以在该主机启动的实际引擎。ARM64 原生运行要求 harness 的 ELF 解释器指向已安装的 bionic linker。

不需要桌面环境、窗口管理器、物理显示器或 GPU。只有核心库和 D-Bus 前端的 Fcitx5 构建不能运行此测试，必须补齐图形模块。

## 运行

在目标主机的源码目录执行：

```sh
LIB=/absolute/path/libfcitx5-wetype.so \
WETYPE_ENGINE_DIR=/absolute/path/arm64 \
bash fcitx5-wetype/e2e/run_visual_e2e.sh
```

测试结束会输出截图、日志和 `result.json` 的目录。也可用 `WETYPE_VISUAL_OUT` 指定一个新的结果目录。`FCITX5_BIN` 可指定 Fcitx5 可执行文件；独立安装的程序还应将对应的 bin 目录加入 PATH，以使用配套的 `fcitx5-remote`。

若图形模块没有安装到系统目录，可用 `WETYPE_VISUAL_ADDON_DATA=/path/to/share` 添加数据搜索路径；其中应包含 `fcitx5/addon/*.conf` 和 `fcitx5/themes/default/`，addon 配置的 Library 应指向对应模块的绝对路径。

## 断言和证据

1. 通过真实 X11 键盘事件输入 `nihao`。
2. 等待可见的 `Fcitx5 Input Window`，核对它的 PID 属于本次启动的 Fcitx5。
3. 截取实际屏幕和候选窗，确认 GTK Entry 尚未收到上屏文本。
4. 用 `=` 翻到下一页，断言候选窗渲染像素发生变化；用 `-` 返回，断言恢复首屏像素。
5. 按空格，断言 GTK Entry 内容恰好是 `你好`，候选窗隐藏，再截图。

关键产物：

- `01-candidates.png`：输入框和真实候选窗。
- `01-candidate-window.png`：首屏候选窗裁剪。
- `01b-next-page.png`：候选翻页后的屏幕。
- `02-committed.png`：文字进入 GTK 输入框后的屏幕。
- `result.json`、`test.log`、`fcitx5.log`、`gtk.log`：结果和诊断记录。

该用例覆盖 X11 + GTK3 的输入与绘制链路。它不代表 Wayland、Qt 或所有应用的兼容性测试；像素变化断言也不替代字体、遮挡、DPI 等专项视觉检查。
