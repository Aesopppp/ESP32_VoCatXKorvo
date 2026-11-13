"""直接运行 MQTT 生产协议和服务逻辑；模拟 broker/NVS，不接触设备凭据。"""
import argparse
from pathlib import Path
import shutil
import subprocess
import re


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cc", default=shutil.which("tcc"))
    args = parser.parse_args()
    if not args.cc:
        raise SystemExit("请用 --cc 指定主机 C 编译器")
    root = Path(__file__).resolve().parents[1]
    output = root / "work/device-mqtt"
    output.mkdir(parents=True, exist_ok=True)
    service = (root / "main/device_mqtt_service.c").read_text(encoding="utf-8")
    service = re.sub(r'^#include .*$', '', service, flags=re.M)
    service = service.replace('extern const char mqtt_ca[] asm("_binary_device_mqtt_ca_pem_start");',
                              'static const char mqtt_ca[] = "test-only-ca";')
    (output / "device_mqtt_service.inc").write_text(service, encoding="utf-8")
    cases = {"protocol": ["tests/device_mqtt_checks.c"],
             "service": ["-Itests/ui_host", "-Iwork/device-mqtt", "tests/device_mqtt_service_checks.c"],
             "missing-ca": ["-DTEST_NO_CA=1", "-Itests/ui_host", "-Iwork/device-mqtt", "tests/device_mqtt_service_checks.c"],
             "undefined-voice": ["-DDEVICE_MQTT_FORWARD_TASK_TO_WS=0", "-Itests/ui_host", "-Iwork/device-mqtt", "tests/device_mqtt_service_checks.c"]}
    for name, sources in cases.items():
        executable = output / f"{name}-checks.exe"
        command = [args.cc, "-std=c11", "-Wall", "-Werror", "-Imanaged_components/espressif__cjson/cJSON",
                   "-o", str(executable), *sources, "main/device_mqtt_protocol.c", "main/device_handshake.c",
                   "managed_components/espressif__cjson/cJSON/cJSON.c"]
        with (output / f"{name}.log").open("w", encoding="utf-8") as log:
            for run in (command, [str(executable)]):
                result = subprocess.run(run, cwd=root, text=True, encoding="utf-8", errors="replace",
                                        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
                print(result.stdout, end="")
                log.write(result.stdout)
                if result.returncode:
                    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
