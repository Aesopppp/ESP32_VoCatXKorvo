"""运行设备 HTTP 握手的生产解析、请求交换和重试策略回归；不连接平台。"""
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
    output = root / "work/device-handshake"
    output.mkdir(parents=True, exist_ok=True)
    executable = output / "handshake-checks.exe"
    sources = [
        "-Imanaged_components/espressif__cjson/cJSON",
        "tests/device_handshake_checks.c",
        "main/device_handshake.c",
        "main/device_handshake_http.c",
        "managed_components/espressif__cjson/cJSON/cJSON.c",
    ]
    with (output / "checks.log").open("w", encoding="utf-8") as log:
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
