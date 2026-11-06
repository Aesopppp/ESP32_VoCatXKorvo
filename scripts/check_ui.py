"""用 TCC 在桌面运行生产手势规则和 LVGL 页面/操作路由，输出四页 PPM。

先执行 idf.py reconfigure 下载依赖，再运行 python scripts/check_ui.py --cc <tcc路径>。
这里只替换 RTOS 和设备传输，不能代替实板触摸、DMA、NVS 和音频验证。
"""
import argparse
from pathlib import Path
import shutil
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=shutil.which("tcc"), help="TCC 可执行文件")
    parser.add_argument("--animation-frames", action="store_true", help="额外导出五种表情各 100 帧 PPM，50 ms/帧")
    options = parser.parse_args()
    compiler = options.cc
    if not compiler:
        raise SystemExit("请用 --cc 指定已安装的 TCC，不需要修改全局 PATH")
    root = Path(__file__).resolve().parents[1]
    lvgl = root / "managed_components/lvgl__lvgl"
    if not (lvgl / "lvgl.h").exists():
        raise SystemExit("请先运行 idf.py reconfigure 下载图形库")
    output = root / "work/ui-checks"
    output.mkdir(parents=True, exist_ok=True)
    display_exe = output / "display.exe"
    subprocess.run([compiler, "-std=c11", "-Wall", "-Werror", "-Itests/display_host", "-Itests/ui_host",
                    "-o", str(display_exe), "tests/display_host/display_checks.c"], cwd=root, check=True)
    subprocess.run([str(display_exe)], cwd=root, check=True)
    subprocess.run([sys.executable, "scripts/generate_face_assets.py", "--source", "assets/face",
                    "--output", str(output / "ui_assets.bin")], cwd=root, check=True)
    decoder_exe = output / "animation.exe"
    subprocess.run([compiler, "-std=c11", "-Wall", "-Werror", "-o", str(decoder_exe),
                    "tests/ui_animation_checks.c", "main/ui_animation.c"], cwd=root, check=True)
    subprocess.run([str(decoder_exe)], cwd=root, check=True)
    policy_exe = output / "policy.exe"
    subprocess.run([compiler, "-std=c11", "-Wall", "-Werror", "-o", str(policy_exe),
                    "tests/ui_policy_checks.c"], cwd=root, check=True)
    subprocess.run([str(policy_exe)], cwd=root, check=True)
    render_exe = output / "render.exe"
    args = ["-std=c11", "-w", "-DLV_CONF_SKIP=1", "-DLV_USE_STDLIB_MALLOC=1",
            "-DLV_USE_OS=0", "-DLV_USE_THEME_DEFAULT=0", "-DLV_FONT_SOURCE_HAN_SANS_SC_16_CJK=1",
            "-Itests/ui_host", "-I" + str(lvgl), "-o", str(render_exe),
            "tests/ui_host/render.c", "main/ui_face.c", "main/ui_animation.c", "main/vocat_ui_font_20.c"]
    args.extend(str(p) for p in sorted((lvgl / "src").rglob("*.c")))
    response = output / "render-args.rsp"
    response.write_text("\n".join('"' + arg.replace("\\", "/") + '"' for arg in args), encoding="utf-8")
    subprocess.run([compiler, "@" + str(response)], cwd=root, check=True)
    # 单独进程模拟忘记烧资源包；菜单/点击仍须可用，不能让语音因 UI 资源缺失而失败。
    subprocess.run([str(render_exe), "--missing-assets"], cwd=root, check=True)
    subprocess.run([str(render_exe)] + (["--animation-frames"] if options.animation_frames else []), cwd=root, check=True)
    print(f"页面预览：{output}（home/settings/confirm/provision.ppm）")


if __name__ == "__main__":
    main()
