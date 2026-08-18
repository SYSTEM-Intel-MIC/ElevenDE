// KWin 脚本：Ctrl+Alt+Delete → sas-screen --show
// 若 KWin.Script.runCommand 不可用（低版本 Plasma），
// 可用系统设置 → 快捷键 手动添加全局快捷键执行 "sas-screen --show"。
registerShortcut(
    "SAS Screen",
    "Show SAS secure attention screen",
    "Control+Alt+Delete",
    function () {
        KWin.Script.runCommand("sas-screen --show");
    }
);