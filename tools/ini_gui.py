"""
Nioh1PerfectGuard - INI 配置界面 (config editor GUI)

设计原则（很重要）：
  * **只改值，不动文件的其他任何东西** —— 注释、分节横幅、空行、键的顺序、行尾符
    （INI 用的是 LF）、是否需要 BOM，全部原样保留。用户说"注释太多不好找"，
    但注释本身是这份 INI 的价值，所以这里绝不重新生成文件，只替换 `键=值` 里的值。
  * **写前校验范围** —— MOD 的 ini_int/ini_float 一旦遇到越界值会拒绝**整组**游戏设置
    （曾经因为 LivingWeaponGaugeMax=0 越界，导致 Enabled 一起归零、MOD 完全旁路）。
    界面按 MOD 自己的范围做校验，从源头上避免这种"改一个键把整个 MOD 关掉"。
  * 只依赖标准库 tkinter，不需要安装任何第三方包（也不写入注册表）。

用法：
    python ini_gui.py [--ini 路径]      打开界面
    python ini_gui.py --selftest        非交互自检：解析→原样回写必须字节一致，
                                        改一个值必须只改那一行（供 build/CI 用）
"""

import argparse
import datetime
import json
import os
import re
import shutil
import sys
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

APP_TITLE = "仁王1 精防 MOD · 配置界面 (Nioh1PerfectGuard)"
INI_NAME = "Nioh1PerfectGuard.ini"
SETTINGS = os.path.join(os.environ.get("APPDATA", os.path.expanduser("~")),
                        "Nioh1PerfectGuard", "gui.json")

# key -> (显示名, 类型, 最小值, 最大值, 说明)
#   类型：bool 勾选框 / int / float / hex(十六进制整数) / str
#   范围与 MOD 源码里的 ini_int / ini_float 一致（越界会被 MOD 拒收）
SCHEMA = [
    ("01 · 基本设置 / General", [
        ("Enabled", "总开关", "bool", 0, 1, "0 = 完全不装断点（旁路）。改成 1 需要重启游戏才生效；其余键都是热加载（约 1 秒）。"),
        ("WindowMs", "精防判定窗口 (ms)", "int", 0, 2000, "按下防御后多少毫秒内发生格挡才算精防。默认 450；调大更宽松（按住防御也算），调小更严格。"),
        ("CancelRecovery", "取消硬直恢复", "bool", 0, 1, "精防后跳过剩余硬直。"),
        ("CancelRecoveryFrames", "跳过的帧数", "int", 0, 120, "配合上一项；30 约为半秒。"),
    ]),
    ("02 · 精防判定 / What counts", [
        ("RequireTimelyGuard", "要求「新按下」", "bool", 0, 1, "1 = 必须是新按下的防御（按住不算）；0 = 只要在窗口内格挡即可。"),
        ("GuardButtonMask", "防御键位掩码", "hex", 0, 0xFFFFFFFF, "手柄按键位。0x0100 = L1；0x0200 = R1 等。改错会导致识别不到防御。"),
        ("ParryButtonMask", "精防触发键（可选）", "hex", 0, 0xFFFF, "0 = 沿用防御键。0x8000 = Y（石火式弹反）、0x2000 = B（化解式）。只开精防判定窗，不取消动作，可与武技同键。"),
        ("PadSlot", "手柄槽位", "int", 0, 3, "0 = 第一个手柄。"),
        ("GuardKeyVK", "键鼠防御键 (VK)", "int", 0, 255, "0 = 不用键鼠。例如 0x02 = 鼠标右键、0x10 = Shift。"),
        ("LearnButtons", "按键学习", "bool", 0, 1, "打开后日志会打印按下的按键码，方便查掩码。"),
        ("KiTrace", "耗精追踪", "bool", 0, 1, "日志里打印格挡耗精细节（体积较大）。"),
        ("DiagDisable", "诊断位掩码", "int", 0, 65535, "逐位关闭诊断输出：16 = 关掉「调用游戏代码」那类诊断。看不懂就别动。"),
        ("BlockEventSource", "格挡事件来源", "int", 0, 3, "2 = 自动（推荐）。其他值用于排查本机识别不到精防的问题。"),
    ]),
    ("03 · 格挡耗精 / Guard Ki cost", [
        ("KiDamageReductionPercent", "格挡耗精减免 %", "int", 0, 100, "100 = 格挡不消耗精力。"),
        ("KiTopUp", "精防回精", "bool", 0, 1, "精防成功后把这次消耗的精力补回来。"),
        ("KiTopUpPreEventMs", "回精参考窗口 (ms)", "int", 0, 1000, "用事件前这段时间的精力值做参考。默认 100。"),
    ]),
    ("04 · 回精 / Ki recovery", [
        ("KiRecoveryMode", "模式", "int", 0, 3, "0 = 关；3 = 推荐的组合模式。"),
        ("FixedRecovery", "固定回复量", "int", 0, 1000, "固定模式下每次回复的精力点数。"),
    ]),
    ("04b · 回血 / HP restore", [
        ("HpRecoveryMode", "模式", "int", 0, 3, "0 = 关；1 = 按百分比；2 = 固定值；3 = 两者取大。"),
        ("HpRestorePercent", "回复 %", "int", 0, 100, "按最大生命的百分比回复。"),
        ("HpRestoreFixed", "固定回复量", "int", 0, 100000, "固定回复的生命点数。"),
    ]),
    ("04c · 精防后的限时增益 / Timed buffs", [
        ("DamageCutPercent", "承受伤害降低 %", "int", 0, 100, "✅ 已实机验证生效。精防后这段时间内受到的伤害按此比例降低（乘算：装备 10% + 本项 50% ≈ 总 55%）。"),
        ("DamageCutMs", "减伤持续 (ms)", "int", 0, 600000, "减伤窗口长度，默认 10000（10 秒）。"),
        ("ArmorBuff", "霸体（无硬直）", "bool", 0, 1, "默认关：实测设置该位未能改变硬直，实现保留待查。"),
        ("ArmorBuffMs", "霸体持续 (ms)", "int", 0, 600000, "霸体窗口长度。"),
        ("SpeedBuffPercent", "移速增益 %", "int", 0, 100, "尚未实现，保持 0。"),
        ("SpeedBuffMs", "移速持续 (ms)", "int", 0, 600000, "尚未实现，保持 10000。"),
    ]),
    ("06 · 九十九槽（精华量表）/ 99 gauge", [
        ("LivingWeaponGaugeOnGuard", "精防攒槽", "bool", 0, 1, "✅ 已验证。不在九十九状态时，精防成功给精华量表加百分比。"),
        ("LivingWeaponGaugePercent", "攒槽 %", "int", 0, 100, "每次精防给量表加多少百分比（默认 10）。"),
        ("LivingWeaponExtendOnGuard", "精防续烧条", "bool", 0, 1, "✅ 已验证。九十九状态（烧条 > 0）中精防成功，延长烧条。"),
        ("LivingWeaponExtendPercent", "续烧条百分点", "int", 0, 100, "按百分点加（烧条满 100）。实测总时长 7~30 秒，所以 35 点 ≈ 延长总时长的 35%。"),
        ("LivingWeaponGaugeOffset", "量表字段偏移 (hex)", "hex", 0, 0x4000, "【高级】量表所在的 int 计数器偏移，默认 0xC0（上限取「偏移 +8」处）。不懂别改。"),
        ("LivingWeaponGaugeMax", "量表上限", "int", 0, 100000, "【高级】0 = 运行时从字段里现读上限（推荐）；填正数则强制用这个值。"),
    ]),
    ("04d · 防御取消当前动作 / Guard cancel", [
        ("CancelActionOnGuard", "防御取消攻击", "bool", 0, 1, "按下防御可立刻取消当前攻击动作。"),
        ("AttackButtonMask", "攻击键位掩码", "hex", 0, 0xFFFFFFFF, "用于判断「按下防御时正在攻击」。0xF000 覆盖常见攻击键。"),
        ("ComboGuardWindowMs", "连段窗口 (ms)", "int", 0, 1000, "连段中允许取消的时间窗口。"),
        ("CancelActionStrictHold", "要求按住", "bool", 0, 1, "1 = 必须按住防御才取消（避免误触）。"),
        ("CancelActionFrames", "跳过帧数", "int", 0, 120, "取消动作时前进的动作帧数。"),
        ("CancelActionRecentMs", "最近动作窗口 (ms)", "int", 0, 5000, "0 = 不限制；只取消最近这段时间内开始的攻击。"),
    ]),
    ("05 · 对敌效果 / Enemy effects", [
        ("EnemyKiDamage", "削减敌精", "float", 0, 100000, "精防成功后对敌人精力造成的额外削减。"),
        ("EnemyHpDamage", "削减敌血", "float", 0, 100000, "精防成功后对敌人生命造成的额外伤害。"),
    ]),
    ("06 · 音效 / Sound", [
        ("SoundEnabled", "音效开关", "bool", 0, 1, "精防成功播放音效。"),
        ("SoundVolume", "音量", "float", 0, 1, "0.0 ~ 1.0。"),
        ("SoundFile", "音频文件", "str", 0, 0, "与 INI 同目录的 wav 文件名。"),
    ]),
    ("07 · 日志 / Logs", [
        ("DiagnosticHotkey", "诊断热键", "bool", 0, 1, "按热键把诊断摘要写进日志。"),
    ]),
]

BANNER = re.compile(r"^\s*;\s*[─\-=#*]{2,}\s*(.+?)\s*[─\-=#*]{2,}\s*$")
LINE = re.compile(r"^(\s*)([A-Za-z][A-Za-z0-9_]*)(\s*=\s*)(.*?)(\s*)$")


# 【高级】键默认折叠，避免新手误改（它们会直接影响识别与字段定位）
ADVANCED = {"LivingWeaponGaugeOffset", "LivingWeaponGaugeMax", "DiagDisable",
            "KiTrace", "LearnButtons", "BlockEventSource"}

# 预设：一键写入常用组合（只改内存中的值，仍需点"保存"才落盘）
PRESETS = [
    ("石火式：Y 精防 (Ronin style)", {"GuardButtonMask": "0x0100", "ParryButtonMask": "0x8000"}),
    ("化解式：B 精防 (Wo Long style)", {"GuardButtonMask": "0x0100", "ParryButtonMask": "0x2000"}),
    ("经典：L1 精防", {"GuardButtonMask": "0x0100", "ParryButtonMask": "0"}),
    ("手感推荐", {"WindowMs": "600", "DamageCutPercent": "50", "DamageCutMs": "10000",
                  "LivingWeaponGaugePercent": "10", "LivingWeaponExtendPercent": "35",
                  "ArmorBuff": "0"}),
]
# ---- 中英切换 / language toggle ------------------------------------------------------
# 用"中文原文 -> 英文"的查表方式翻译，SCHEMA 本身不必改动：查不到就沿用原文，
# 所以界面永远不会出现空白项，未翻译的长说明会自然回落到中文（界面里会标注覆盖范围）。
LANG = {"cur": "zh"}
EN = {
    # UI 外壳
    "预设 / Presets:": "Presets:",
    "打开…": "Open…", "打开文件夹": "Open folder", "重新载入": "Reload",
    "保存": "Save", "退出": "Quit", "恢复备份…": "Restore backup…",
    "选择要恢复的备份（会先备份当前文件）：": "Pick a backup to restore (the current file is backed up first):",
    "恢复": "Restore", "语言 / Language": "语言 / Language",
    "高级 / Advanced（改动前请先读说明）": "Advanced (read the notes before changing)",
    "其他 / Other": "Other",
    # 分节标题
    "01 · 基本设置 / General": "01 · General",
    "02 · 精防判定 / What counts": "02 · What counts as a perfect guard",
    "03 · 格挡耗精 / Guard Ki cost": "03 · Guard Ki cost",
    "04 · 回精 / Ki recovery": "04 · Ki recovery",
    "04b · 回血 / HP restore": "04b · HP restore",
    "04c · 精防后的限时增益 / Timed buffs": "04c · Timed buffs after a perfect guard",
    "06 · 九十九槽（精华量表）/ 99 gauge": "06 · 99 gauge (Living Weapon)",
    "04d · 防御取消当前动作 / Guard cancel": "04d · Guard cancels the current action",
    "05 · 对敌效果 / Enemy effects": "05 · Enemy effects",
    "06 · 音效 / Sound": "06 · Sound",
    "07 · 日志 / Logs": "07 · Logs",
    # 常用键的标签
    "总开关": "Master switch", "精防判定窗口 (ms)": "Perfect-guard window (ms)",
    "取消硬直恢复": "Cancel recovery", "跳过的帧数": "Frames skipped",
    "要求「新按下」": "Require a fresh press", "防御键位掩码": "Guard button mask",
    "精防触发键（可选）": "Parry trigger (optional)", "手柄槽位": "Pad slot",
    "键鼠防御键 (VK)": "Guard key VK (keyboard)", "按键学习": "Learn buttons",
    "耗精追踪": "Ki trace", "诊断位掩码": "Diagnostics mask",
    "格挡事件来源": "Block event source", "格挡耗精减免 %": "Guard Ki cost reduction %",
    "精防回精": "Ki top-up on parry", "回精参考窗口 (ms)": "Ki top-up reference (ms)",
    "模式": "Mode", "固定回复量": "Fixed recovery",
    "回复 %": "Restore %", "固定回复量 ": "Fixed restore",
    "承受伤害降低 %": "Damage taken reduction %", "减伤持续 (ms)": "Damage cut duration (ms)",
    "霸体（无硬直）": "Armour (no hit-stun)", "霸体持续 (ms)": "Armour duration (ms)",
    "移速增益 %": "Move speed %", "移速持续 (ms)": "Move speed duration (ms)",
    "精防攒槽": "Gauge gain on parry", "攒槽 %": "Gauge gain %",
    "精防续烧条": "Extend the 99 burn", "续烧条百分点": "Burn points added",
    "防御取消攻击": "Guard cancels attacks", "攻击键位掩码": "Attack button mask",
    "连段窗口 (ms)": "Combo window (ms)", "要求按住": "Require hold",
    "最近动作窗口 (ms)": "Recent action window (ms)",
    "削减敌精": "Enemy Ki damage", "削减敌血": "Enemy HP damage",
    "音效开关": "Sound", "音量": "Volume", "音频文件": "Audio file",
    "诊断热键": "Diagnostic hotkey",
}


# 英文长说明：按"键名"索引（比逐字匹配中文原文安全得多），查不到就回落到中文说明。
EN_HINT = {
    "Enabled": "0 = install no breakpoints at all (bypass). Changing this one needs a game restart; every other key hot-reloads within ~1s.",
    "WindowMs": "How long after a fresh press a block still counts as a perfect guard. Bigger = more forgiving.",
    "CancelRecovery": "Skip the remaining recovery frames after a perfect guard.",
    "CancelRecoveryFrames": "Frames skipped for the option above (30 is about half a second).",
    "RequireTimelyGuard": "1 = the block must happen within WindowMs of a FRESH press; 0 = every block counts (testing).",
    "GuardButtonMask": "Guard button as standard XInput bits: 0x0100 = L1/LB, 0x0200 = R1/RB, 0x1000/0x2000/0x4000/0x8000 = A/B/X/Y.",
    "ParryButtonMask": "0 = the guard button decides. 0x8000 = Y (Rise of the Ronin style parry), 0x2000 = B (Wo Long style). Opens the timing window only: it never cancels an action, so a martial skill and a parry can share the key. Uses no input injection, so B keeps picking items up normally.",
    "PadSlot": "Which controller slot to read (0-3). Keep 0 for a single controller.",
    "GuardKeyVK": "Virtual-key code for a keyboard/mouse guard button (0 = unused). e.g. 0x02 = right mouse, 0x10 = Shift.",
    "LearnButtons": "Logs every button you press, so you can find the right mask.",
    "KiTrace": "Extra logging around the guard Ki cost (verbose).",
    "DiagDisable": "Bit mask that silences diagnostics one by one. 16 = the engine-call ones. Leave 0 if unsure.",
    "BlockEventSource": "2 = automatic (recommended). Other values help diagnose a machine where perfect guards are not detected.",
    "KiDamageReductionPercent": "100 = blocking costs no Ki at all.",
    "KiTopUp": "Refund the Ki a perfect guard consumed.",
    "KiTopUpPreEventMs": "How far back to sample Ki for that refund (default 100ms).",
    "KiRecoveryMode": "0 = off; 3 = the recommended combined mode.",
    "FixedRecovery": "Ki restored per trigger in fixed mode.",
    "HpRecoveryMode": "0 = off; 1 = percent; 2 = fixed; 3 = whichever is larger.",
    "HpRestorePercent": "HP restored as a percentage of maximum.",
    "HpRestoreFixed": "HP restored as a flat amount.",
    "DamageCutPercent": "Verified in game. Damage taken is multiplied by (1 - this/100) during the window. It multiplies with your gear reduction (10% gear + 50% here is about 55% total).",
    "DamageCutMs": "Length of the damage-reduction window (default 10000 = 10s).",
    "ArmorBuff": "Off by default: setting the bit did not change hit-stun in testing. The implementation is harmless and stays available.",
    "ArmorBuffMs": "Length of the armour window.",
    "SpeedBuffPercent": "Not implemented yet; keep 0.",
    "SpeedBuffMs": "Not implemented yet; keep 10000.",
    "LivingWeaponGaugeOnGuard": "Verified. While NOT in the 99 state, a perfect guard adds to the amrita/99 gauge.",
    "LivingWeaponGaugePercent": "How much of the gauge one perfect guard adds (default 10).",
    "LivingWeaponExtendOnGuard": "Verified. While the 99 state is active (burn bar > 0), a perfect guard extends the burn.",
    "LivingWeaponExtendPercent": "Added in points (the bar is 100). Full burn lasts 7-30s depending on gear, so 35 points is about +35% duration.",
    "LivingWeaponGaugeOffset": "[Advanced] Byte offset of the gauge int counter (default 0xC0; the maximum is read from offset+8). Do not change unless you know why.",
    "LivingWeaponGaugeMax": "[Advanced] 0 = read the maximum from the field at runtime (recommended); a positive value forces it.",
    "CancelActionOnGuard": "Pressing guard cancels the current attack immediately.",
    "AttackButtonMask": "Used to tell whether an attack was in progress when guard was pressed. 0xF000 covers the usual attack buttons.",
    "ComboGuardWindowMs": "How long into a combo a cancel is still allowed.",
    "CancelActionStrictHold": "1 = guard must be held, so a brush against the button cannot cancel.",
    "CancelActionFrames": "Action frames advanced when cancelling.",
    "CancelActionRecentMs": "0 = no limit; otherwise only attacks started within this many ms are cancelled.",
    "EnemyKiDamage": "Extra Ki damage dealt to the enemy after a perfect guard.",
    "EnemyHpDamage": "Extra HP damage dealt to the enemy after a perfect guard.",
    "SoundEnabled": "Play a sound when a perfect guard lands.",
    "SoundVolume": "0.0 to 1.0.",
    "SoundFile": "wav file name, looked up next to the INI.",
    "DiagnosticHotkey": "Press the hotkey to write a diagnostic summary into the log.",
}


def H(key, hint):
    """English long note for a key, falling back to the Chinese one."""
    if LANG["cur"] == "en":
        return EN_HINT.get(key, hint)
    return hint

def L(s):
    """Translate one UI string (falling back to the original)."""
    if LANG["cur"] == "en":
        return EN.get(s, s)
    return s
class IniFile:
    """逐行保存的 INI：只替换值，其他一切都原样留下。"""

    def __init__(self, path):
        with open(path, "rb") as f:
            raw = f.read()
        self.bom = raw.startswith(b"\xef\xbb\xbf")
        text = raw[3:].decode("utf-8") if self.bom else raw.decode("utf-8")
        self.eol = "\r\n" if "\r\n" in text else "\n"
        self.trailing_nl = text.endswith("\n")
        self.lines = text.split(self.eol) if self.eol in text else [text]
        if self.trailing_nl and self.lines and self.lines[-1] == "":
            self.lines.pop()
        self.values = {}
        self.index = {}
        for i, ln in enumerate(self.lines):
            m = LINE.match(ln)
            if m:
                self.values[m.group(2)] = m.group(4)
                self.index[m.group(2)] = i
        self.original = self.text()

    def text(self):
        t = self.eol.join(self.lines)
        if self.trailing_nl:
            t += self.eol
        return t

    def set(self, key, value):
        i = self.index.get(key)
        if i is None:
            return False
        m = LINE.match(self.lines[i])
        self.lines[i] = m.group(1) + m.group(2) + m.group(3) + value
        self.values[key] = value
        return True

    def group_of(self, key):
        """键所在的分节标题（用于界面分组）。"""
        i = self.index.get(key)
        if i is None:
            return "其他 / Other"
        for j in range(i, -1, -1):
            m = BANNER.match(self.lines[j])
            if m:
                return m.group(1)
        return "其他 / Other"

    def save(self, path):
        data = self.text().encode("utf-8")
        if self.bom:
            data = b"\xef\xbb\xbf" + data
        tmp = path + ".tmp"
        with open(tmp, "wb") as f:
            f.write(data)
        os.replace(tmp, path)


def coerce(kind, text):
    text = (text or "").strip()
    if kind == "bool":
        return 1 if text not in ("0", "") else 0
    if kind == "hex":
        return int(text, 16)
    if kind == "int":
        return int(text, 10)
    if kind == "float":
        return float(text)
    return text


def render(kind, value):
    if kind == "bool":
        return "1" if str(value).strip() not in ("0", "") else "0"
    if kind == "hex":
        return "0x%X" % int(value)
    if kind == "float":
        return ("%g" % float(value))
    return str(value)


def find_ini(explicit=None):
    if explicit and os.path.isfile(explicit):
        return os.path.abspath(explicit)
    try:
        with open(SETTINGS, "r", encoding="utf-8") as f:
            last = json.load(f).get("ini")
        if last and os.path.isfile(last):
            return last
    except Exception:
        pass
    # When frozen (PyInstaller) __file__ points inside a temp extraction dir, so search
    # relative to the exe itself instead.
    if getattr(sys, "frozen", False):
        here = os.path.dirname(os.path.abspath(sys.executable))
    else:
        here = os.path.dirname(os.path.abspath(__file__))
    for c in (here,
              os.path.join(here, "..", "mods", "Nioh1PerfectGuard"),
              os.path.join(here, ".."),
              os.path.join(here, "..", "..", "mod"),
              os.path.join(here, "..", ".."),
              os.path.join(here, "..", "mod")):
        p = os.path.abspath(os.path.join(c, INI_NAME))
        if os.path.isfile(p):
            return p
    return None


def remember(ini_path):
    try:
        os.makedirs(os.path.dirname(SETTINGS), exist_ok=True)
        with open(SETTINGS, "w", encoding="utf-8") as f:
            json.dump({"ini": ini_path}, f)
    except Exception:
        pass


class App:
    def __init__(self, root, ini_path):
        self.root = root
        self.path = ini_path
        self.ini = IniFile(ini_path)
        self.vars = {}
        root.title(APP_TITLE)
        root.geometry("980x720")
        root.minsize(760, 520)

        bar = ttk.Frame(root, padding=(10, 8))
        bar.pack(fill="x")
        self.lbl = ttk.Label(bar, text=self.path, foreground="#333")
        self.lbl.pack(side="left")
        ttk.Button(bar, text=L("打开…"), command=self.open_other).pack(side="right")
        ttk.Button(bar, text="打开文件夹", command=self.open_folder).pack(side="right", padx=6)
        ttk.Button(bar, text="重新载入", command=self.reload).pack(side="right")

        pre = ttk.Frame(root, padding=(10, 0, 10, 6))
        pre.pack(fill="x")
        ttk.Label(pre, text=L("预设 / Presets:")).pack(side="left")
        for name, kv in PRESETS:
            ttk.Button(pre, text=name, command=lambda kv=kv: self.apply_preset(kv)).pack(side="left", padx=4)
        ttk.Button(pre, text="语言 / Language", command=self.toggle_lang).pack(side="right")

        outer = ttk.Frame(root)
        outer.pack(fill="both", expand=True)
        self.canvas = tk.Canvas(outer, highlightthickness=0)
        sb = ttk.Scrollbar(outer, orient="vertical", command=self.canvas.yview)
        self.body = ttk.Frame(self.canvas)
        self.canvas.create_window((0, 0), window=self.body, anchor="nw")
        self.canvas.configure(yscrollcommand=sb.set)
        self.canvas.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        self.body.bind("<Configure>",
                       lambda e: self.canvas.configure(scrollregion=self.canvas.bbox("all")))
        root.bind_all("<MouseWheel>",
                      lambda e: self.canvas.yview_scroll(int(-e.delta / 120), "units"))

        self.build()

        foot = ttk.Frame(root, padding=(10, 8))
        foot.pack(fill="x")
        self.status = ttk.Label(foot, text="就绪。修改后点「保存」，游戏内约 1 秒热加载（Enabled 需要重启）。",
                                foreground="#555")
        self.status.pack(side="left")
        ttk.Button(foot, text="退出", command=root.destroy).pack(side="right")
        ttk.Button(foot, text="恢复备份…", command=self.restore).pack(side="right", padx=6)
        ttk.Button(foot, text="保存", command=self.save).pack(side="right")

    # ---------- 构建界面 ----------
    def build(self):
        for w in self.body.winfo_children():
            w.destroy()
        self.vars.clear()
        deferred = []
        groups = {}
        order = []
        for title, keys in SCHEMA:
            groups[title] = keys
            order.append(title)
        # 界面上没列出的键（未来新增的）也要能改
        listed = {k for _, ks in SCHEMA for k, *_ in ks}
        extra = [(k, k, "str", 0, 0, "（此键未在界面中预定义，直接填值）")
                 for k in self.ini.values if k not in listed]
        if extra:
            groups["其他 / Other"] = extra
            order.append("其他 / Other")

        for title in order:
            ttk.Label(self.body, text=L(title), font=("Segoe UI", 10, "bold"),
                      padding=(12, 12, 8, 4)).pack(anchor="w")
            for key, label, kind, lo, hi, hint in groups[title]:
                if key not in self.ini.values:
                    continue
                if key in ADVANCED:
                    deferred.append((key, label, kind, lo, hi, hint))
                    continue
                row = ttk.Frame(self.body, padding=(22, 2))
                row.pack(fill="x")
                ttk.Label(row, text=L(label), width=26).pack(side="left")
                var = tk.StringVar(value=self.ini.values[key])
                self.vars[key] = (var, kind, lo, hi, label)
                if kind == "bool":
                    cb = ttk.Checkbutton(row, variable=var, onvalue="1", offvalue="0")
                    cb.pack(side="left")
                else:
                    width = 12 if kind != "str" else 28
                    ttk.Entry(row, textvariable=var, width=width).pack(side="left")
                    if kind != "str":
                        ttk.Label(row, text="范围 %s ~ %s" % (
                            ("0x%X" % lo if kind == "hex" else lo),
                            ("0x%X" % hi if kind == "hex" else hi)),
                            foreground="#888").pack(side="left", padx=8)
                ttk.Label(row, text=key, foreground="#999").pack(side="left", padx=8)
                ttk.Label(row, text=H(key, hint), foreground="#666", wraplength=560,
                          justify="left").pack(side="left", padx=6)

        if deferred:
            ttk.Label(self.body, text="高级 / Advanced（改动前请先读说明）",
                      font=("Segoe UI", 10, "bold"), padding=(12, 16, 8, 4)).pack(anchor="w")
            for key, label, kind, lo, hi, hint in deferred:
                row = ttk.Frame(self.body, padding=(22, 2))
                row.pack(fill="x")
                ttk.Label(row, text=L(label), width=26).pack(side="left")
                var = tk.StringVar(value=self.ini.values[key])
                self.vars[key] = (var, kind, lo, hi, label)
                ttk.Entry(row, textvariable=var, width=12).pack(side="left")
                ttk.Label(row, text=key, foreground="#999").pack(side="left", padx=8)
                ttk.Label(row, text=H(key, hint), foreground="#666", wraplength=560,
                          justify="left").pack(side="left", padx=6)

    # ---------- 动作 ----------
    def toggle_lang(self):
        LANG["cur"] = "en" if LANG["cur"] == "zh" else "zh"
        self.build()
        self.status.config(text="Language: English (full)" if LANG["cur"] == "en" else "语言：中文")

    def apply_preset(self, kv):
        hit = 0
        for k, v in kv.items():
            if k in self.vars:
                self.vars[k][0].set(v)
                hit += 1
        self.status.config(text="已套用预设 %d 项，请点「保存」写入文件。" % hit)
    def reload(self):
        try:
            self.ini = IniFile(self.path)
            self.build()
            self.status.config(text="已重新载入：" + datetime.datetime.now().strftime("%H:%M:%S"))
        except Exception as e:
            messagebox.showerror(APP_TITLE, "重新载入失败：\n%s" % e)

    def open_other(self):
        p = filedialog.askopenfilename(title="选择 Nioh1PerfectGuard.ini",
                                       filetypes=[("INI", "*.ini"), ("全部", "*.*")])
        if p:
            self.path = p
            self.lbl.config(text=p)
            remember(p)
            self.reload()

    def open_folder(self):
        try:
            os.startfile(os.path.dirname(self.path))
        except Exception:
            pass

    def save(self):
        changed = {}
        for key, (var, kind, lo, hi, label) in self.vars.items():
            raw = var.get()
            try:
                v = coerce(kind, raw)
            except Exception:
                messagebox.showerror(APP_TITLE, "【%s】的值无法识别：%r" % (label, raw))
                return
            if kind != "str" and not (lo <= v <= hi):
                messagebox.showerror(
                    APP_TITLE,
                    "【%s】的值 %s 超出允许范围 %s ~ %s。\n\n"
                    "MOD 对越界值会拒绝整组设置（曾经因此让总开关一起归零），所以这里先拦住。"
                    % (label, raw, lo, hi))
                return
            new = render(kind, v)
            if new != self.ini.values.get(key):
                changed[key] = new
        if not changed:
            self.status.config(text="没有改动。")
            return
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        bak = "%s.%s.bak" % (self.path, stamp)
        try:
            shutil.copy2(self.path, bak)
            for k, v in changed.items():
                self.ini.set(k, v)
            self.ini.save(self.path)
        except Exception as e:
            messagebox.showerror(APP_TITLE, "保存失败：\n%s" % e)
            return
        self.ini = IniFile(self.path)
        self.build()
        self.status.config(text="已保存 %d 项（备份 %s）%s" % (
            len(changed), os.path.basename(bak),
            "；总开关 Enabled 的改动需要重启游戏" if "Enabled" in changed else ""))
        messagebox.showinfo(APP_TITLE, "已写入 %s\n\n备份：%s\n改动：%s\n\n"
                                       "游戏内约 1 秒自动热加载（Enabled 例外，需重启）。"
                            % (os.path.basename(self.path), os.path.basename(bak),
                               ", ".join(sorted(changed))))

    def restore(self):
        d = os.path.dirname(self.path)
        baks = sorted([f for f in os.listdir(d) if f.startswith(INI_NAME + ".") and f.endswith(".bak")])
        if not baks:
            messagebox.showinfo(APP_TITLE, "同目录下没有找到备份（*.bak）。")
            return
        win = tk.Toplevel(self.root)
        win.title("恢复备份")
        win.geometry("460x300")
        ttk.Label(win, text="选择要恢复的备份（会先备份当前文件）：",
                  padding=8).pack(anchor="w")
        lb = tk.Listbox(win)
        for b in reversed(baks):
            lb.insert("end", b)
        lb.pack(fill="both", expand=True, padx=8)
        lb.selection_set(0)

        def do():
            sel = lb.curselection()
            if not sel:
                return
            src = os.path.join(d, lb.get(sel[0]))
            stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
            shutil.copy2(self.path, "%s.%s.bak" % (self.path, stamp))
            shutil.copy2(src, self.path)
            name = lb.get(sel[0])
            win.destroy()
            self.reload()
            self.status.config(text="已从 %s 恢复。" % name)
        ttk.Button(win, text="恢复", command=do).pack(pady=8)


def selftest(ini_path):
    """非交互自检：原样回写必须字节一致；改一个值只改那一行。"""
    ok = True
    with open(ini_path, "rb") as f:
        before = f.read()
    ini = IniFile(ini_path)
    tmp = ini_path + ".selftest"
    ini.save(tmp)
    with open(tmp, "rb") as f:
        after = f.read()
    if before != after:
        print("FAIL: 未改动时回写不是字节一致")
        ok = False
    else:
        print("PASS: 未改动时回写字节一致 (%d B, LF=%s, BOM=%s)"
              % (len(before), ini.eol == "\n", ini.bom))
    key = "WindowMs" if "WindowMs" in ini.values else sorted(ini.values)[0]
    old = ini.values[key]
    ini.set(key, "999")
    ini.save(tmp)
    txt = open(tmp, "r", encoding="utf-8-sig").read()
    hit = [l for l in txt.split(ini.eol) if l.strip().startswith(key + "=")]
    if hit and "999" in hit[0] and txt.count(old) >= 1:
        print("PASS: 改动只影响目标行 (%s=%s -> 999)" % (key, old))
    else:
        print("FAIL: 改动没有正确落到目标行")
        ok = False
    os.remove(tmp)
    print("SELFTEST", "OK" if ok else "FAILED")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=APP_TITLE)
    ap.add_argument("--ini", help="INI 路径（默认自动查找并记住上次使用的）")
    ap.add_argument("--selftest", action="store_true", help="非交互自检")
    a = ap.parse_args()
    path = find_ini(a.ini)
    if a.selftest:
        if not path:
            print("FAIL: 找不到 INI")
            return 1
        return selftest(path)
    if not path:
        r = tk.Tk()
        r.withdraw()
        path = filedialog.askopenfilename(title="请选择 Nioh1PerfectGuard.ini",
                                          filetypes=[("INI", "*.ini"), ("全部", "*.*")])
        r.destroy()
        if not path:
            return 1
    remember(path)
    root = tk.Tk()
    App(root, path)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
