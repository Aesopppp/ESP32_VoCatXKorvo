"""运行生产输入分包/暂停恢复与回复状态机的桌面回归；不连接设备或平台。"""
import argparse
from pathlib import Path
import shutil
import subprocess


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=shutil.which("tcc"), help="本机 TCC 路径")
    args = parser.parse_args()
    if not args.cc:
        raise SystemExit("请用 --cc 指定已安装的 TCC")
    root = Path(__file__).resolve().parents[1]
    output = root / "work/speech-handoff"
    output.mkdir(parents=True, exist_ok=True)
    # 直接编译 app_main 的生产交接函数，依赖由主机测试替换；不复制另一套流程。
    # 这些文件级函数的闭括号独占行。签名或结构变化时显式失败，要求同步测试入口。
    source = (root / "main/app_main.c").read_text(encoding="utf-8")
    declaration_end = source.index("} input_handoff;") + len("} input_handoff;")
    declaration_start = source.rindex("static struct {", 0, declaration_end)
    sections = [source[declaration_start:declaration_end]]
    for name in ("reset_input_handoff", "process_reply_offer"):
        start = source.index(f"static void {name}(void)\n{{")
        end = source.index("\n}\n", start) + 3
        sections.append(source[start:end])
    (output / "app_handoff.inc").write_text("\n\n".join(sections), encoding="utf-8")
    cases = {
        "response-checks": ["-Itests/host", "-Imanaged_components/espressif__cjson/cJSON",
                            "tests/response_stream_checks.c", "main/proto_service.c",
                            "main/response_stream.c", "managed_components/espressif__cjson/cJSON/cJSON.c"],
        "upload-checks": ["-Itests/audio_host", "-Itests/ui_host", "-Itests/host",
                          "tests/audio_host/upload_checks.c"],
        "handoff-checks": ["tests/input_handoff_checks.c", "main/response_stream.c"],
        "app-handoff-checks": ["-Itests/audio_host", "-Itests/ui_host", "-Itests/host",
                               "-Iwork/speech-handoff", "tests/audio_host/app_handoff_checks.c"],
    }
    for name, sources in cases.items():
        executable = output / (name + ".exe")
        with (output / (name + ".log")).open("w", encoding="utf-8") as log:
            for command in ([args.cc, "-std=c11", "-Wall", "-Werror", "-o", str(executable), *sources],
                            [str(executable)]):
                result = subprocess.run(command, cwd=root, text=True, encoding="utf-8",
                                        errors="replace", stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                log.write(result.stdout)
                print(result.stdout, end="")
                if result.returncode:
                    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
