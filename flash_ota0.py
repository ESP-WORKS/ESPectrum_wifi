# VSCode/PlatformIO Upload with the 4MB table:
# nvs, otadata, factory (0x10000 / 0x120000), ota_0 (0x130000 / 0x2C0000), spiffs
# Firmware is 2.2MB so it only fits in ota_0. Factory is too small.
Import("env")
import os

OTA0_OFFSET = "0x130000"


def before_upload(source, target, env):
    build = env.subst("$BUILD_DIR")
    boot = os.path.join(build, "bootloader.bin")
    if not os.path.isfile(boot):
        alt = os.path.join(build, "bootloader", "bootloader.bin")
        if os.path.isfile(alt):
            boot = alt
    part = os.path.join(build, "partitions.bin")
    firmware = os.path.join(build, "firmware.bin")

    images = []
    if os.path.isfile(boot):
        images += ["0x1000", '"%s"' % boot.replace("\\", "/")]
    if os.path.isfile(part):
        images += ["0x8000", '"%s"' % part.replace("\\", "/")]
    images += [OTA0_OFFSET, '"%s"' % firmware.replace("\\", "/")]

    env.Replace(
        UPLOADCMD=" ".join(
            [
                '"$PYTHONEXE"',
                '"$UPLOADER"',
                "--chip",
                "esp32",
                "--port",
                '"$UPLOAD_PORT"',
                "--baud",
                "$UPLOAD_SPEED",
                "--before",
                "default_reset",
                "--after",
                "hard_reset",
                "write_flash",
                "-z",
                "--flash_size",
                "4MB",
            ]
            + images
        )
    )
    print("ESPectrum: VSCode upload -> " + " ".join(images))


env.AddPreAction("upload", before_upload)
env.Replace(
    UPLOADCMD=(
        '"$PYTHONEXE" "$UPLOADER" --chip esp32 --port "$UPLOAD_PORT" '
        "--baud $UPLOAD_SPEED --before default_reset --after hard_reset "
        "write_flash -z --flash_size 4MB "
        "0x1000 \"$BUILD_DIR/bootloader.bin\" "
        "0x8000 \"$BUILD_DIR/partitions.bin\" "
        "%s \"$BUILD_DIR/firmware.bin\"" % OTA0_OFFSET
    )
)
print("ESPectrum: firmware goes to ota_0 @ %s (factory is only 1.18MB)." % OTA0_OFFSET)
