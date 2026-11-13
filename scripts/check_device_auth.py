"""运行生产凭据准备、WS 发送和主任务门禁回归；不请求平台、不读取私有配置。"""
import argparse
from pathlib import Path
import re
import shutil
import subprocess


def extract(source, name):
    match = re.search(r"^[^\n;{}]*\b" + re.escape(name) + r"\([^;{}]*\)\s*\{", source, re.M)
    if not match:
        raise RuntimeError(f"生产函数未找到：{name}")
    # 本组函数不包含带花括号的字符串；提取原函数，不在测试中复制业务算法。
    depth = 1
    end = match.end()
    while depth and end < len(source):
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    if depth:
        raise RuntimeError(f"生产函数边界不完整：{name}")
    return source[match.start():end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=shutil.which("tcc"), help="本机 TCC 路径")
    args = parser.parse_args()
    if not args.cc:
        raise SystemExit("请用 --cc 指定已安装的 TCC")
    root = Path(__file__).resolve().parents[1]
    output = root / "work/device-ws-auth"
    output.mkdir(parents=True, exist_ok=True)
    groups = {
        "device_handshake_service.c": ("device_handshake_service_get_configuration",
            "device_handshake_service_is_enabled", "device_handshake_service_renew_credentials",
            "device_handshake_service_prepare_websocket"),
        "ws_service.c": ("release_credentials", "ws_service_set_credentials", "ws_service_authenticate"),
        "app_main.c": ("handle_voice_auth_failure", "retry_voice_connection"),
    }
    for file, names in groups.items():
        source = (root / "main" / file).read_text(encoding="utf-8")
        (output / (file + ".inc")).write_text(
            "\n\n".join(extract(source, name) for name in names), encoding="utf-8")
    executable = output / "device-auth-checks.exe"
    sources = ["-Itests/audio_host", "-Itests/ui_host", "-Itests/host",
               "-Imanaged_components/espressif__cjson/cJSON", "-Iwork/device-ws-auth",
               "tests/device_auth_checks.c", "main/device_handshake.c", "main/proto_service.c",
               "managed_components/espressif__cjson/cJSON/cJSON.c"]
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
